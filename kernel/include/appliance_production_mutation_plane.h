#ifndef HFOC_APPLIANCE_PRODUCTION_MUTATION_PLANE_H
#define HFOC_APPLIANCE_PRODUCTION_MUTATION_PLANE_H

#include "appliance_management_authorization.h"
#include "appliance_production_auth.h"
#include "appliance_production_management_plane.h"
#include "network_config_transaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-367A production network/service mutation operations.
 *
 * This caller-owned, allocation-free composition binds the ITEM-365
 * production request gate (`appliance_production_auth`), the ITEM-366
 * read-only management plane (`appliance_production_management_plane`) and
 * the existing network configuration transaction
 * (`network_config_transaction`) so an authorized administrator can mutate
 * production network configuration without introducing a second device
 * registry, route table, or state store.
 *
 * Every mutation is two-phase and never partially applied:
 *   1. an authorized POST/PUT stages a candidate configuration and issues a
 *      single-use confirmation token with a bounded lifetime;
 *   2. only a second authorized request carrying that exact token commits
 *      the candidate, while an explicit cancel or an expired window rolls it
 *      back and leaves the active configuration untouched.
 *
 * Mutations fail closed: an unpublished credential, unpublished management
 * evidence, a missing or wrong confirmation token, a rejected body, or a
 * stopped plane applies nothing. Every attempt is recorded in a bounded,
 * caller-owned audit trail. A mutation target whose executor is not bound
 * (for example a service action without a service executor) is refused
 * honestly instead of being reported as applied, and readiness is published
 * as UNAVAILABLE or DEGRADED rather than a fictitious READY.
 */

#define APPLIANCE_PRODUCTION_MUTATION_PLANE_VERSION 1U
#define APPLIANCE_PRODUCTION_MUTATION_PLANE_OUTPUT_MAX 192U
#define APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CAPACITY 16U
/*
 * ITEM-504: the confirmation window is measured in milliseconds, the unit
 * `hfoc_lwip_time_now()` publishes and the unit `pending_tick`/`now_tick` are
 * compared in.  The token is single-use and the pending candidate rolls back
 * when the window closes.  A person confirms on the panel - the certificate
 * import of ITEM-503 and the routes of ITEM-504 both hand the token to the
 * browser - so the window has to outlive the page that prints it and the
 * click that returns it.  The previous value read as 300 ticks and was in
 * fact 300 ms: the token expired between the response that issued it and the
 * next request, so no HTTP client, and certainly no operator, could ever
 * confirm.  Five minutes is bounded, single-use and long enough to read the
 * page that names what will change.
 */
#define APPLIANCE_PRODUCTION_MUTATION_PLANE_CONFIRMATION_WINDOW_MS 300000U

enum appliance_production_mutation_plane_state {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PREPARED = 2,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_UNAVAILABLE = 3,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_READY = 4,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_DEGRADED = 5,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_PENDING_CONFIRMATION = 6,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_STOPPED = 7,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_STATE_FAILED = 8,
};

enum appliance_production_mutation_plane_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_NULL = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_UNAVAILABLE = 4,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_DENIED = 5,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_PENDING_CONFIRMATION = 6,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLIED = 7,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_CANCELLED = 8,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_EXPIRED = 9,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_NO_PENDING = 10,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_BAD_CONFIRMATION = 11,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_UNSUPPORTED_TARGET = 12,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_APPLY_FAILED = 13,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_BUFFER_TOO_SMALL = 14,
};

enum appliance_production_mutation_plane_audit_outcome {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_NONE = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_ALLOWED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_DENIED = 2,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_UNAVAILABLE = 3,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CANCELLED = 4,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_EXPIRED = 5,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_APPLY_FAILED = 6,
};

struct appliance_production_mutation_plane_audit_entry {
    uint64_t sequence;
    uint64_t tick;
    uint64_t token;
    uint32_t mutation;
    uint32_t outcome;
    uint32_t session_id;
    uint32_t principal_id;
};

/*
 * ITEM-503: the executor for the certificate-import mutation.
 *
 * The plane owns the two-phase state machine - authorization, the single-use
 * token, the window, the audit ring - and delegates the bytes to one bound
 * executor.  The network mutation is executed by the borrowed
 * `network_config_transaction`; the certificate import is executed by the
 * caller's upload composition, which holds the candidate, the clock and the
 * certificate store owner.  An unbound executor is refused as an unsupported
 * target, never reported as applied.
 */
enum appliance_production_mutation_plane_certificate_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_REFUSED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_APPLY_FAILED = 2,
};

struct appliance_production_mutation_plane_certificate_executor {
    void *context;
    enum appliance_production_mutation_plane_certificate_status (*stage)(
        void *context,
        const uint8_t *body,
        size_t body_length);
    enum appliance_production_mutation_plane_certificate_status (*commit)(
        void *context);
    void (*rollback)(void *context);
};

/*
 * ITEM-504: the executor for the routing-configuration mutation.
 *
 * The candidate pair lives in the caller's route configuration (the route
 * table owns the grammar); the plane owns the two-phase state exactly as it
 * does for the certificate.  The stage receives one form body and holds the
 * candidate, the commit replaces the applied table whole, and the rollback
 * drops the candidate; an unbound executor is refused as an unsupported
 * target, never reported as applied.
 */
enum appliance_production_mutation_plane_routing_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_REFUSED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ROUTING_APPLY_FAILED = 2,
};

struct appliance_production_mutation_plane_routing_executor {
    void *context;
    enum appliance_production_mutation_plane_routing_status (*stage)(
        void *context,
        const uint8_t *body,
        size_t body_length);
    enum appliance_production_mutation_plane_routing_status (*commit)(
        void *context);
    void (*rollback)(void *context);
};

/*
 * ITEM-505: the executor for the network-configuration mutation.
 *
 * The network mutation is the one mutation whose commit does not rebind the
 * live interface: the operator stages a candidate on the panel, confirms it,
 * and the machine applies it at the next boot.  The plane keeps the two-phase
 * state exactly as it does for the certificate and the routes; the caller's
 * executor owns the candidate, the durable record and the honest reading of
 * what the interface still runs.  A composition that binds no executor falls
 * back to the borrowed `network_config_transaction` (RAM only) only when one
 * was passed to `init`, so an unbound network mutation is still refused as an
 * unsupported target instead of being reported as persisted.
 */
enum appliance_production_mutation_plane_network_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_REFUSED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_NETWORK_APPLY_FAILED = 2,
};

struct appliance_production_mutation_plane_network_executor {
    void *context;
    enum appliance_production_mutation_plane_network_status (*stage)(
        void *context,
        const uint8_t *body,
        size_t body_length);
    enum appliance_production_mutation_plane_network_status (*commit)(
        void *context);
    void (*rollback)(void *context);
};

/*
 * ITEM-505 increment 3: the executor for the system-hostname mutation.
 *
 * The hostname mutation writes the system configuration record the next boot
 * resolves as the machine's identity and as the name in its certificate; the
 * running listener keeps the identity it booted with.  It is one form field,
 * so the plane's two-phase state is the whole state machine and the caller's
 * executor owns the candidate and the durable record, exactly as the network
 * executor does.  An unbound executor is refused as an unsupported target,
 * never reported as persisted.
 */
enum appliance_production_mutation_plane_system_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_REFUSED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_APPLY_FAILED = 2,
};

struct appliance_production_mutation_plane_system_executor {
    void *context;
    enum appliance_production_mutation_plane_system_status (*stage)(
        void *context,
        const uint8_t *body,
        size_t body_length);
    enum appliance_production_mutation_plane_system_status (*commit)(
        void *context);
    void (*rollback)(void *context);
};

/*
 * ITEM-506 increment 6: the executor for the one account's credential change.
 *
 * The change rewrites the administrator credential record and, on success, the
 * owner republishes the verifier and closes every issued session - including
 * the session that confirmed the change.  Stage proves the current password and
 * holds the new pair; commit is the only place the durable record is written,
 * and the owner's setter is the only writer, so the invalidation cannot be
 * bypassed.  An unbound executor is refused as an unsupported target, never
 * reported as persisted.
 */
enum appliance_production_mutation_plane_account_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_REFUSED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_APPLY_FAILED = 2,
};

struct appliance_production_mutation_plane_account_executor {
    void *context;
    enum appliance_production_mutation_plane_account_status (*stage)(
        void *context,
        const uint8_t *body,
        size_t body_length);
    enum appliance_production_mutation_plane_account_status (*commit)(
        void *context);
    void (*rollback)(void *context);
};

/*
 * ITEM-507 increment 4: the executor for the name -> certificate/key table.
 *
 * One mutation imports a certificate for an exact name, deletes a declared
 * name, or declares what an unknown name is served.  Stage parses the
 * percent-encoded PEM fields, validates the pair through the ITEM-503 import
 * gate against the declared name, and holds the candidate - including the
 * private key, which the commit zeroes on every path.  Commit rewrites the
 * whole durable record and proves the write; the running listener keeps the
 * table it was configured with until the reboot it promises the operator.  An
 * unbound executor is refused as an unsupported target, never reported as
 * persisted.
 */
enum appliance_production_mutation_plane_certificate_name_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_REFUSED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_APPLY_FAILED = 2,
    /*
     * ITEM-649: the ITEM-503 import gate refused because the wall clock is
     * not yet validated.  It is its own status so the serving path can name
     * the cause instead of collapsing every refusal into REFUSED.
     */
    APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_BAD_CLOCK = 3,
};

struct appliance_production_mutation_plane_certificate_name_executor {
    void *context;
    enum appliance_production_mutation_plane_certificate_name_status (*stage)(
        void *context,
        const uint8_t *body,
        size_t body_length);
    enum appliance_production_mutation_plane_certificate_name_status (*commit)(
        void *context);
    void (*rollback)(void *context);
};

/*
 * ITEM-508 increment 34: the executor for the durable ACME control.
 *
 * One mutation enables ACME, disables it - clearing any pending issuance
 * request with it - or requests one issuance while ACME is enabled.  Stage
 * parses the one action field and holds the candidate; commit writes the
 * control record, proves the write by reading it back, and only then reports
 * applied.  The running service reads the committed state on its next poll;
 * nothing durable is written for a candidate the operator did not confirm.  An
 * unbound executor is refused as an unsupported target, never reported as
 * persisted.
 */
enum appliance_production_mutation_plane_acme_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_REFUSED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_ACME_APPLY_FAILED = 2,
};

struct appliance_production_mutation_plane_acme_executor {
    void *context;
    enum appliance_production_mutation_plane_acme_status (*stage)(
        void *context,
        const uint8_t *body,
        size_t body_length);
    enum appliance_production_mutation_plane_acme_status (*commit)(
        void *context);
    void (*rollback)(void *context);
};

/*
 * ITEM-516: the executor for the management gate's after-boot policy.
 *
 * The panel may only ever close the gate - the safe direction - so stage
 * parses the one action field, refuses anything that is not `closed` by name
 * (the serving composition holds the refusal word the HTTP reply quotes) and
 * holds the candidate; commit writes the default closed through the system
 * record's own setter and proves the write.  Opening is not reachable here at
 * all: it is the local console's, behind a proven local secret.  An unbound
 * executor is refused as an unsupported target, never reported as persisted.
 */
enum appliance_production_mutation_plane_panel_gate_status {
    APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_OK = 0,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_REFUSED = 1,
    APPLIANCE_PRODUCTION_MUTATION_PLANE_PANEL_GATE_APPLY_FAILED = 2,
};

struct appliance_production_mutation_plane_panel_gate_executor {
    void *context;
    enum appliance_production_mutation_plane_panel_gate_status (*stage)(
        void *context,
        const uint8_t *body,
        size_t body_length);
    enum appliance_production_mutation_plane_panel_gate_status (*commit)(
        void *context);
    void (*rollback)(void *context);
};

struct appliance_production_mutation_plane {
    bool initialized;
    uint32_t version;
    enum appliance_production_mutation_plane_state state;
    enum appliance_production_mutation_plane_status last_status;
    struct appliance_production_auth *auth;
    struct appliance_production_management_plane *management;
    struct network_config_transaction *network;
    /* ITEM-505: the bound network-configuration executor, if any. */
    struct appliance_production_mutation_plane_network_executor
        network_executor;
    bool network_executor_bound;
    /* ITEM-503: the bound certificate-import executor, if any. */
    struct appliance_production_mutation_plane_certificate_executor
        certificate;
    bool certificate_bound;
    /* ITEM-504: the bound routing-configuration executor, if any. */
    struct appliance_production_mutation_plane_routing_executor routing;
    bool routing_bound;
    /* ITEM-505 increment 3: the bound system-hostname executor, if any. */
    struct appliance_production_mutation_plane_system_executor system;
    bool system_bound;
    /* ITEM-506 increment 6: the bound credential-change executor, if any. */
    struct appliance_production_mutation_plane_account_executor account;
    bool account_bound;
    /* ITEM-507 increment 4: the bound certificate-name executor, if any. */
    struct appliance_production_mutation_plane_certificate_name_executor
        certificate_name;
    bool certificate_name_bound;
    /* ITEM-649: the named stage refusal the serving path may publish. */
    const char *certificate_name_refusal;
    /* ITEM-508 increment 34: the bound ACME control executor, if any. */
    struct appliance_production_mutation_plane_acme_executor acme;
    bool acme_bound;
    /* ITEM-516: the bound management-gate policy executor, if any. */
    struct appliance_production_mutation_plane_panel_gate_executor panel_gate;
    bool panel_gate_bound;
    uint64_t confirmation_sequence;
    uint64_t pending_token;
    uint64_t pending_tick;
    uint64_t confirmation_window;
    enum appliance_management_authorization_mutation pending_mutation;
    uint32_t applied_count;
    uint32_t denied_count;
    uint32_t cancelled_count;
    uint32_t expired_count;
    uint32_t unavailable_count;
    bool audit_published;
    uint64_t audit_next_sequence;
    uint32_t audit_count;
    uint32_t audit_head;
    struct appliance_production_mutation_plane_audit_entry audit[
        APPLIANCE_PRODUCTION_MUTATION_PLANE_AUDIT_CAPACITY];
};

enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_init(
    struct appliance_production_mutation_plane *plane,
    struct appliance_production_auth *auth,
    struct appliance_production_management_plane *management,
    struct network_config_transaction *network);
/*
 * Bind the certificate-import executor.  The structure is copied; the context
 * stays owned by the caller and must outlive the binding.  A NULL argument or
 * a NULL function clears the binding and the mutation is then refused as an
 * unsupported target.
 */
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_certificate(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_certificate_executor
        *executor);
/*
 * Bind the routing-configuration executor.  The structure is copied; the
 * context stays owned by the caller and must outlive the binding.  A NULL
 * argument or a NULL function clears the binding and the mutation is then
 * refused as an unsupported target.
 */
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_routing(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_routing_executor
        *executor);
/*
 * Bind the network-configuration executor.  The structure is copied; the
 * context stays owned by the caller and must outlive the binding.  A NULL
 * argument or a NULL function clears the binding; the mutation then falls
 * back to the borrowed `network_config_transaction`, and with neither bound
 * it is refused as an unsupported target.
 */
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_network_executor(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_network_executor
        *executor);
/*
 * ITEM-505 increment 3: bind the system-hostname executor.  The structure is
 * copied; the context stays owned by the caller and must outlive the binding.
 * A NULL argument or a NULL function clears the binding and the mutation is
 * then refused as an unsupported target.
 */
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_system(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_system_executor
        *executor);
/*
 * ITEM-506 increment 6: bind the credential-change executor.  The structure is
 * copied; the context stays owned by the caller and must outlive the binding.
 * A NULL argument or a NULL function clears the binding and the mutation is
 * then refused as an unsupported target.
 */
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_account(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_account_executor
        *executor);
/*
 * ITEM-507 increment 4: bind the certificate-name executor.  The structure is
 * copied; the context stays owned by the caller and must outlive the binding.
 * A NULL argument or a NULL function clears the binding and the mutation is
 * then refused as an unsupported target.
 */
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_certificate_name(
    struct appliance_production_mutation_plane *plane,
    const struct
        appliance_production_mutation_plane_certificate_name_executor
            *executor);
/*
 * ITEM-508 increment 34: bind the ACME control executor.  The structure is
 * copied; the context stays owned by the caller and must outlive the binding.
 * A NULL argument or a NULL function clears the binding and the mutation is
 * then refused as an unsupported target.
 */
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_acme(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_acme_executor *executor);
/*
 * ITEM-516: bind the management-gate policy executor.  The structure is
 * copied; the context stays owned by the caller and must outlive the binding.
 * A NULL argument or a NULL function clears the binding and the mutation is
 * then refused as an unsupported target.
 */
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_bind_panel_gate(
    struct appliance_production_mutation_plane *plane,
    const struct appliance_production_mutation_plane_panel_gate_executor
        *executor);
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_refresh(
    struct appliance_production_mutation_plane *plane);
bool appliance_production_mutation_plane_ready(
    const struct appliance_production_mutation_plane *plane);
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_request(
    struct appliance_production_mutation_plane *plane,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    enum appliance_management_authorization_method method,
    const uint8_t *target,
    uint32_t target_length,
    const char *body,
    size_t body_length,
    uint64_t confirmation_token,
    bool cancel,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_stop(
    struct appliance_production_mutation_plane *plane);
enum appliance_production_mutation_plane_status
appliance_production_mutation_plane_fail_closed(
    struct appliance_production_mutation_plane *plane);
enum appliance_production_mutation_plane_state
appliance_production_mutation_plane_state(
    const struct appliance_production_mutation_plane *plane);
bool appliance_production_mutation_plane_has_pending(
    const struct appliance_production_mutation_plane *plane);
uint64_t appliance_production_mutation_plane_pending_token(
    const struct appliance_production_mutation_plane *plane);
/*
 * ITEM-649: the named refusal of the most recent certificate-name request, or
 * NULL when the refusal has no more specific name than REFUSED.  The string is
 * a static literal owned by the plane; the caller must not free it.
 */
const char *appliance_production_mutation_plane_certificate_name_refusal(
    const struct appliance_production_mutation_plane *plane);
enum appliance_management_authorization_mutation
appliance_production_mutation_plane_pending_mutation(
    const struct appliance_production_mutation_plane *plane);
uint32_t appliance_production_mutation_plane_applied_count(
    const struct appliance_production_mutation_plane *plane);
uint32_t appliance_production_mutation_plane_denied_count(
    const struct appliance_production_mutation_plane *plane);
uint32_t appliance_production_mutation_plane_cancelled_count(
    const struct appliance_production_mutation_plane *plane);
uint32_t appliance_production_mutation_plane_expired_count(
    const struct appliance_production_mutation_plane *plane);
uint32_t appliance_production_mutation_plane_unavailable_count(
    const struct appliance_production_mutation_plane *plane);
uint32_t appliance_production_mutation_plane_audit_count(
    const struct appliance_production_mutation_plane *plane);
bool appliance_production_mutation_plane_audit_entry(
    const struct appliance_production_mutation_plane *plane,
    uint32_t index,
    struct appliance_production_mutation_plane_audit_entry *entry_out);
const char *appliance_production_mutation_plane_state_string(
    enum appliance_production_mutation_plane_state state);
const char *appliance_production_mutation_plane_status_string(
    enum appliance_production_mutation_plane_status status);
const char *appliance_production_mutation_plane_audit_outcome_string(
    enum appliance_production_mutation_plane_audit_outcome outcome);

#endif
