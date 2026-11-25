#include "appliance_management_authorization.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool appliance_management_authorization_exact_equal(
    const uint8_t *target,
    uint32_t target_length,
    const char *expected)
{
    uint32_t index;
    size_t expected_length = strlen(expected);

    if (target_length != expected_length) {
        return false;
    }
    for (index = 0U; index < target_length; ++index) {
        if (target[index] != (uint8_t)expected[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_management_authorization_is_management_target(
    const uint8_t *target,
    uint32_t target_length)
{
    static const char prefix[] = "/api";
    static const char panel[] = "/panel";

    if (target_length >= sizeof(prefix) - 1U &&
        appliance_management_authorization_exact_equal(
            target,
            (uint32_t)(sizeof(prefix) - 1U),
            prefix) &&
        (target_length == sizeof(prefix) - 1U ||
         target[sizeof(prefix) - 1U] == '/')) {
        return true;
    }
    /*
     * ITEM-384: the rendered panel is a management surface too. Only the
     * exact targets below resolve to a route, so any other /panel...
     * target stays UNKNOWN_ROUTE and is denied by default.
     */
    if (target_length >= sizeof(panel) - 1U &&
        appliance_management_authorization_exact_equal(
            target,
            (uint32_t)(sizeof(panel) - 1U),
            panel) &&
        (target_length == sizeof(panel) - 1U ||
         target[sizeof(panel) - 1U] == '/')) {
        return true;
    }
    return false;
}

/*
 * ITEM-509 increment 8: `/panel/help/<id>` is a Help topic page.  The suffix
 * is one catalog id - the same shape the panel parser enforces (letters,
 * digits, `-` and `_`, at most 31 bytes) - so the route stays bounded and the
 * renderer resolves the id against the one help catalog, naming an unknown id
 * instead of falling through to another route.
 */
static bool appliance_management_authorization_panel_help_topic(
    const uint8_t *target,
    uint32_t target_length)
{
    static const char prefix[] = "/panel/help/";
    uint32_t index;
    uint32_t topic_length;

    if (target == NULL || target_length <= sizeof(prefix) - 1U) {
        return false;
    }
    if (memcmp(target, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    topic_length = target_length - (uint32_t)(sizeof(prefix) - 1U);
    if (topic_length > 31U) {
        return false;
    }
    for (index = (uint32_t)(sizeof(prefix) - 1U);
         index < target_length;
         ++index) {
        const uint8_t value = target[index];
        const bool allowed =
            (value >= (uint8_t)'a' && value <= (uint8_t)'z') ||
            (value >= (uint8_t)'A' && value <= (uint8_t)'Z') ||
            (value >= (uint8_t)'0' && value <= (uint8_t)'9') ||
            value == (uint8_t)'-' ||
            value == (uint8_t)'_';

        if (!allowed) {
            return false;
        }
    }
    return true;
}

/*
 * The exact panel targets the ITEM-384 section model and the ITEM-382
 * about/licenses page publish, plus the ITEM-509 help topic pages.  Nothing
 * else under /panel resolves to a route.
 */
static bool appliance_management_authorization_panel_target(
    const uint8_t *target,
    uint32_t target_length)
{
    static const char *const paths[] = {
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_PATH,
        "/panel/network",
        "/panel/services",
        "/panel/storage",
        "/panel/system",
        "/panel/security",
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ROUTING_PATH,
        "/panel/diagnostics",
        "/panel/updates",
        "/panel/recovery",
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LICENSES_PATH,
        /* ITEM-506 increment 6: the rendered Account page. */
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH,
        /* ITEM-507 increment 5: the rendered Certificates page. */
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_CERTIFICATES_PATH,
        /* ITEM-508 increment 34: the rendered ACME control page. */
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACME_PATH,
        /* ITEM-509 increment 8: the rendered Help index. */
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_HELP_PATH,
        /* ITEM-513: the nav's one-click language switch. */
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LANGUAGE_PATH,
        /* ITEM-516: the rendered management-gate page. */
        APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH,
    };
    size_t index;

    for (index = 0U; index < sizeof(paths) / sizeof(paths[0]); ++index) {
        if (appliance_management_authorization_exact_equal(
                target, target_length, paths[index])) {
            return true;
        }
    }
    return appliance_management_authorization_panel_help_topic(
        target, target_length);
}

static enum appliance_management_authorization_route
appliance_management_authorization_route_for_target(
    const uint8_t *target,
    uint32_t target_length)
{
    if (appliance_management_authorization_exact_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_DISCOVERY_PATH)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY;
    }
    if (appliance_management_authorization_exact_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS;
    }
    if (appliance_management_authorization_exact_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_STATUS_PATH)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS;
    }
    if (appliance_management_authorization_exact_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_NETWORK_PATH)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG;
    }
    if (appliance_management_authorization_exact_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_SERVICE_PATH)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS;
    }
    /*
     * ITEM-401: the aggregate read-only health target is resolved through the
     * same status-read entry as the system status.  Without this mapping the
     * ITEM-400 owner served the body but the gate had no route for it, so a
     * valid administrator session was default-denied with 403.
     */
    if (appliance_management_authorization_exact_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_HEALTH_PATH)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS;
    }
    if (appliance_management_authorization_panel_target(
            target, target_length)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL;
    }
    return APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
}

static enum appliance_management_authorization_status
appliance_management_authorization_decision_valid(
    const struct appliance_admin_auth_decision *decision)
{
    if (decision->authenticated) {
        if (decision->principal_id != APPLIANCE_ADMIN_PRINCIPAL_ID ||
            decision->session_id < 1U ||
            decision->session_id > APPLIANCE_ADMIN_AUTH_SESSION_ID_MAX ||
            decision->credential_generation == 0U) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE;
        }
    } else if (decision->principal_id != 0U ||
               decision->session_id != 0U ||
               decision->credential_generation != 0U) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE;
    }
    return APPLIANCE_MANAGEMENT_AUTHORIZATION_OK;
}

enum appliance_management_authorization_status
appliance_management_authorization_policy_init(
    struct appliance_management_authorization_policy *policy)
{
    static const uint8_t admin_role[1] = {
        AUTHORIZATION_ROLE_ADMIN
    };
    static const uint8_t status_read[1] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    static const uint8_t config_write[1] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    /*
     * ITEM-367A: the read entries stay strictly status-read. Mutating
     * command ids are separate entries that require exactly the
     * configuration-write capability, because `authorization_check` demands
     * every capability listed by the matched entry and the mutation caller
     * presents only that one. Adding configuration-write to a read entry
     * would break the read-only request path, and requiring status-read on a
     * mutation entry would deny the configuration-write caller.
     */
    struct authorization_entry entries[17];
    uint32_t index;
    uint32_t route_ids[17] = {
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_NETWORK_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_SERVICE_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_UPDATE_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_RECOVERY_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_FACTORY_RESET_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ROUTING_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_SYSTEM_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACCOUNT_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_NAME_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACME_MUTATION,
        APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_GATE_MUTATION,
    };
    static const bool route_mutable[17] = {
        false,
        false,
        false,
        false,
        true,
        true,
        true,
        true,
        true,
        false,
        true,
        true,
        true,
        /* ITEM-506 increment 6: the account mutation writes configuration. */
        true,
        /* ITEM-507 increment 4: the certificate-name mutation writes
         * configuration. */
        true,
        /* ITEM-508 increment 34: the ACME control mutation writes the durable
         * control record the next boot reads. */
        true,
        /* ITEM-516: the gate mutation writes the durable after-boot policy the
         * next boot reads. */
        true,
    };

    if (policy == NULL) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL;
    }
    memset(policy, 0, sizeof(*policy));
    memset(entries, 0, sizeof(entries));
    for (index = 0U; index < 17U; ++index) {
        entries[index].version = AUTHORIZATION_VERSION;
        entries[index].command_id = route_ids[index];
        entries[index].role_count = 1U;
        memcpy(entries[index].roles,
               admin_role,
               sizeof(admin_role));
        entries[index].capability_count = 1U;
        memcpy(entries[index].capabilities,
               route_mutable[index] ? config_write : status_read,
               sizeof(status_read));
    }
    if (authorization_matrix_init(
            &policy->matrix,
            entries,
            sizeof(entries) / sizeof(entries[0])) != AUTHORIZATION_OK) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE;
    }
    policy->version = APPLIANCE_MANAGEMENT_AUTHORIZATION_VERSION;
    policy->initialized = true;
    return APPLIANCE_MANAGEMENT_AUTHORIZATION_OK;
}

enum appliance_management_authorization_status
appliance_management_authorization_check_request(
    const struct appliance_management_authorization_policy *policy,
    const struct appliance_admin_auth_decision *decision,
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_authorization_route *route_out)
{
    enum appliance_management_authorization_route route;
    enum appliance_management_authorization_status status;
    enum authorization_status authorization_status;
    static const uint8_t admin_role[1] = {
        AUTHORIZATION_ROLE_ADMIN
    };
    static const uint8_t status_read[1] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };

    if (policy == NULL || decision == NULL ||
        target == NULL || route_out == NULL) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL;
    }
    if (policy->version != APPLIANCE_MANAGEMENT_AUTHORIZATION_VERSION) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_VERSION;
    }
    if (!policy->initialized || !policy->matrix.initialized ||
        policy->matrix.version != AUTHORIZATION_VERSION) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE;
    }
    if (!appliance_management_authorization_is_management_target(
            target,
            target_length)) {
        *route_out = APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT;
    }

    route = appliance_management_authorization_route_for_target(
        target,
        target_length);
    *route_out = route;
    if (route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE;
    }
    if (method == HTTP_METHOD_NONE) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE;
    }
    /*
     * ITEM-513: every management target is read-only over GET/HEAD except the
     * nav's one-click language switch, which is POST by design and changes
     * only the browser's own cookie.  It is the one declared POST target on
     * the panel route; a POST anywhere else stays METHOD_NOT_ALLOWED exactly
     * as it was.
     */
    if (method != HTTP_METHOD_GET && method != HTTP_METHOD_HEAD &&
        !(method == HTTP_METHOD_POST &&
          appliance_management_authorization_exact_equal(
              target,
              target_length,
              APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LANGUAGE_PATH))) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
    }

    status = appliance_management_authorization_decision_valid(decision);
    if (status != APPLIANCE_MANAGEMENT_AUTHORIZATION_OK) {
        return status;
    }
    if (route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_OK;
    }
    if (!decision->authenticated) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED;
    }

    authorization_status = authorization_check(
        &policy->matrix,
        (uint32_t)route,
        admin_role,
        sizeof(admin_role) / sizeof(admin_role[0]),
        status_read,
        sizeof(status_read) / sizeof(status_read[0]));
    if (authorization_status == AUTHORIZATION_OK) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_OK;
    }
    if (authorization_status == AUTHORIZATION_MISSING_ROLE) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE;
    }
    if (authorization_status == AUTHORIZATION_MISSING_CAPABILITY) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY;
    }
    return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION;
}

enum appliance_management_authorization_status
appliance_management_authorization_check_mutation_request(
    const struct appliance_management_authorization_policy *policy,
    const struct appliance_admin_auth_decision *decision,
    enum appliance_management_authorization_method method,
    const uint8_t *target,
    uint32_t target_length,
    enum appliance_management_authorization_mutation *mutation_out)
{
    enum appliance_management_authorization_route route;
    enum appliance_management_authorization_status status;
    enum authorization_status authorization_status;
    uint32_t command_id;
    static const uint8_t admin_role[1] = {
        AUTHORIZATION_ROLE_ADMIN
    };
    static const uint8_t config_write[1] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };

    if (policy == NULL || decision == NULL ||
        target == NULL || mutation_out == NULL) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL;
    }
    *mutation_out = APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE;
    if (policy->version != APPLIANCE_MANAGEMENT_AUTHORIZATION_VERSION) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_VERSION;
    }
    if (!policy->initialized || !policy->matrix.initialized ||
        policy->matrix.version != AUTHORIZATION_VERSION) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE;
    }
    if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST &&
        method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_PUT) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
    }
    if (!appliance_management_authorization_is_management_target(
            target,
            target_length)) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT;
    }

    if (appliance_management_authorization_exact_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_RECOVERY_PATH)) {
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_RECOVERY_ACTION;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_RECOVERY_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_FACTORY_RESET_PATH)) {
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_FACTORY_RESET;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_FACTORY_RESET_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
            target,
            target_length,
            APPLIANCE_MANAGEMENT_AUTHORIZATION_UPDATE_PATH)) {
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_UPDATE_ACCEPT;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_UPDATE_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_CERTIFICATE_PATH) ||
               appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SECURITY_PATH)) {
        /*
         * ITEM-503: the panel form and the API target are the same mutation.
         * Only POST is accepted; the panel has no PUT form and the executor is
         * one two-phase path, not two.
         */
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_NETWORK_PATH)) {
        /*
         * ITEM-505: the panel's network form is the same `NETWORK_CONFIG`
         * mutation the API target already resolves to (that target keeps its
         * POST/PUT envelope through the route table below).  The form itself
         * is POST-only, exactly like the certificate and routing forms.
         */
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_NETWORK_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTES_PATH) ||
               appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ROUTING_PATH)) {
        /*
         * ITEM-504: the panel form and the API target are the same mutation.
         * Only POST is accepted; the route table's candidate pair is one
         * two-phase path, not two.
         */
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ROUTING_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_SYSTEM_PATH) ||
               appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SYSTEM_PATH)) {
        /*
         * ITEM-505 increment 3: the panel form and the scriptable API target
         * are the same mutation.  Only POST is accepted; the GET of
         * `/api/v1/system` keeps resolving to the system-status read route,
         * because this explicit branch is reached only from the mutation gate
         * and the read path never consults it.
         */
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_SYSTEM_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_ACCOUNT_PATH) ||
               appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH)) {
        /*
         * ITEM-506 increment 6: the panel form and the scriptable API target
         * are the same credential change.  Only POST is accepted; the GET of
         * `/panel/account` keeps resolving to the rendered Account page, and
         * the API target has no read route at all.
         */
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACCOUNT_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_CERTIFICATE_NAME_PATH) ||
               appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_CERTIFICATES_PATH)) {
        /*
         * ITEM-507 increments 4-5: the panel form and the scriptable API
         * target are the same name -> certificate/key mutation.  Only POST is
         * accepted; the GET of `/panel/certificates` keeps resolving to the
         * rendered page, and the API target has no read route.
         */
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_NAME_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_ACME_PATH) ||
               appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACME_PATH)) {
        /*
         * ITEM-508 increment 34: the panel control page and the scriptable API
         * target are the same enable/disable/request mutation.  Only POST is
         * accepted; the GET of `/panel/acme` keeps resolving to the rendered
         * control page, and the API target has no read route.
         */
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACME_MUTATION;
    } else if (appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_GATE_PATH) ||
               appliance_management_authorization_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH)) {
        /*
         * ITEM-516: the panel page and the scriptable API target are the same
         * two-phase mutation, and it can only ever close the gate.  Only POST
         * is accepted; the GET of `/panel/gate` keeps resolving to the
         * rendered page, and the API target has no read route.
         */
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_GATE_MUTATION;
    } else {
    route = appliance_management_authorization_route_for_target(
        target,
        target_length);
    if (route == APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG) {
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_NETWORK_MUTATION;
    } else if (route ==
               APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS) {
        if (method != APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_POST) {
            return APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED;
        }
        *mutation_out =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SERVICE_ACTION;
        command_id =
            APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_SERVICE_MUTATION;
    } else {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE;
    }
    }

    status = appliance_management_authorization_decision_valid(decision);
    if (status != APPLIANCE_MANAGEMENT_AUTHORIZATION_OK) {
        return status;
    }
    if (!decision->authenticated) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED;
    }

    authorization_status = authorization_check(
        &policy->matrix,
        command_id,
        admin_role,
        sizeof(admin_role) / sizeof(admin_role[0]),
        config_write,
        sizeof(config_write) / sizeof(config_write[0]));
    if (authorization_status == AUTHORIZATION_OK) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_OK;
    }
    if (authorization_status == AUTHORIZATION_MISSING_ROLE) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE;
    }
    if (authorization_status == AUTHORIZATION_MISSING_CAPABILITY) {
        return APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY;
    }
    return APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION;
}

const char *appliance_management_authorization_status_string(
    enum appliance_management_authorization_status status)
{
    switch (status) {
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_OK:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_OK";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_NULL";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_STATE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_VERSION:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_VERSION";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_NOT_MANAGEMENT";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_UNKNOWN_ROUTE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_UNAUTHENTICATED";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_ROLE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_DENIED_CAPABILITY";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_BAD_AUTHORIZATION";
    default:
        return NULL;
    }
}

const char *appliance_management_authorization_route_string(
    enum appliance_management_authorization_route route)
{
    switch (route) {
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_DISCOVERY";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SYSTEM_STATUS";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_CONFIG";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NETWORK_STATUS";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_SERVICE_STATUS";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL:
        return "APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL";
    default:
        return NULL;
    }
}

bool appliance_management_authorization_mutation_has_panel_confirmation(
    enum appliance_management_authorization_mutation mutation)
{
    switch (mutation) {
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG:
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT:
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG:
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME:
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL:
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME:
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL:
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE:
        return true;
    default:
        return false;
    }
}

const char *appliance_management_authorization_mutation_string(
    enum appliance_management_authorization_mutation mutation)
{
    switch (mutation) {
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NONE:
        return "NONE";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG:
        return "NETWORK_CONFIG";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SERVICE_ACTION:
        return "SERVICE_ACTION";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_UPDATE_ACCEPT:
        return "UPDATE_ACCEPT";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_RECOVERY_ACTION:
        return "RECOVERY_ACTION";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_FACTORY_RESET:
        return "FACTORY_RESET";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_IMPORT:
        return "CERTIFICATE_IMPORT";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG:
        return "ROUTING_CONFIG";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME:
        return "SYSTEM_HOSTNAME";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL:
        return "ACCOUNT_CREDENTIAL";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME:
        return "CERTIFICATE_NAME";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL:
        return "ACME_CONTROL";
    case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE:
        return "PANEL_GATE";
    default:
        return "UNKNOWN";
    }
}
