/*
 * ITEM-385 management reachability policy. See the header for the model; this
 * translation unit is a pure policy evaluator with no kernel dependencies so it
 * can be proved by a host test and consumed by the management composition.
 */
#include "appliance_management_reachability.h"

#include <stddef.h>
#include <string.h>

static bool reachability_address_is_wildcard(const uint8_t *address)
{
    size_t index;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH;
         ++index) {
        if (address[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool reachability_address_equal(const uint8_t *left,
                                       const uint8_t *right)
{
    return memcmp(left, right,
                  APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH) == 0;
}

static bool reachability_mode_valid(
    enum appliance_management_reachability_mode mode)
{
    return mode == APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY ||
           mode ==
               APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE ||
           mode == APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN;
}

enum appliance_management_reachability_status
appliance_management_reachability_policy_validate(
    const struct appliance_management_reachability_policy *policy)
{
    uint32_t index;
    uint32_t other;

    if (policy == NULL) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_NULL;
    }
    if (policy->version != APPLIANCE_MANAGEMENT_REACHABILITY_VERSION) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_BAD_VERSION;
    }
    if (!reachability_mode_valid(policy->mode)) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_BAD_MODE;
    }
    if (policy->management_interface == 0U) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_BAD_INTERFACE;
    }
    if (policy->allowlist_count >
        APPLIANCE_MANAGEMENT_REACHABILITY_ALLOWLIST_MAX) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST;
    }
    if (policy->mode != APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN &&
        (policy->allowlist_count != 0U ||
         policy->require_client_certificate)) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST;
    }
    /*
     * An opt-in without an explicit source list would read an empty allowlist
     * as "any peer", the opposite of what the mode promises, so it is refused
     * instead of guessed.
     */
    if (policy->mode == APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN &&
        policy->allowlist_count == 0U) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST;
    }
    for (index = 0U; index < policy->allowlist_count; ++index) {
        if (reachability_address_is_wildcard(policy->allowlist[index])) {
            return APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ADDRESS;
        }
        for (other = index + 1U; other < policy->allowlist_count; ++other) {
            if (reachability_address_equal(policy->allowlist[index],
                                           policy->allowlist[other])) {
                return APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST;
            }
        }
    }
    return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
}

enum appliance_management_reachability_status
appliance_management_reachability_selection_init(
    struct appliance_management_reachability_selection *selection,
    const struct appliance_management_reachability_policy *policy)
{
    enum appliance_management_reachability_status status;

    if (selection == NULL || policy == NULL) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_NULL;
    }
    if (selection->published) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_ALREADY_PUBLISHED;
    }
    status = appliance_management_reachability_policy_validate(policy);
    if (status != APPLIANCE_MANAGEMENT_REACHABILITY_OK) {
        return status;
    }
    memset(selection, 0, sizeof(*selection));
    selection->published = true;
    selection->version = APPLIANCE_MANAGEMENT_REACHABILITY_VERSION;
    selection->policy = *policy;
    return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
}

bool appliance_management_reachability_selection_published(
    const struct appliance_management_reachability_selection *selection)
{
    return selection != NULL && selection->published &&
           selection->version == APPLIANCE_MANAGEMENT_REACHABILITY_VERSION;
}

enum appliance_management_reachability_readiness
appliance_management_reachability_readiness(
    const struct appliance_management_reachability_selection *selection)
{
    if (!appliance_management_reachability_selection_published(selection)) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_UNAVAILABLE;
    }
    if (selection->policy.mode ==
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED;
    }
    if (selection->policy.management_interface ==
        APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_NONE) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED;
    }
    return APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_READY;
}

enum appliance_management_reachability_status
appliance_management_reachability_evaluate(
    const struct appliance_management_reachability_selection *selection,
    const struct appliance_management_reachability_peer *peer,
    enum appliance_management_reachability_verdict *verdict)
{
    const struct appliance_management_reachability_policy *policy;

    if (selection == NULL || peer == NULL || verdict == NULL) {
        return APPLIANCE_MANAGEMENT_REACHABILITY_NULL;
    }

    /* The local console is the break-glass path: always allowed. */
    if (peer->console) {
        *verdict =
            APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_CONSOLE_ALLOWED;
        return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
    }

    if (!appliance_management_reachability_selection_published(selection)) {
        *verdict =
            APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_UNPUBLISHED;
        return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
    }
    policy = &selection->policy;

    if (policy->mode ==
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY) {
        *verdict =
            APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_CONSOLE_ONLY;
        return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
    }

    if (peer->interface_index == policy->management_interface &&
        (reachability_address_is_wildcard(policy->management_address) ||
         reachability_address_equal(policy->management_address,
                                    peer->source_address))) {
        *verdict = APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED;
        return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
    }

    if (policy->mode != APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN) {
        *verdict =
            APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE;
        return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
    }

    {
        uint32_t index;
        bool allowlisted = false;

        for (index = 0U; index < policy->allowlist_count; ++index) {
            if (reachability_address_equal(policy->allowlist[index],
                                           peer->source_address)) {
                allowlisted = true;
                break;
            }
        }
        if (!allowlisted) {
            *verdict = APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NOT_ALLOWLISTED;
            return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
        }
    }
    if (policy->require_client_certificate &&
        !peer->client_certificate_present) {
        *verdict = APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NO_CLIENT_CERTIFICATE;
        return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
    }
    *verdict = APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_REMOTE_OPT_IN_ALLOWED;
    return APPLIANCE_MANAGEMENT_REACHABILITY_OK;
}

bool appliance_management_reachability_verdict_allowed(
    enum appliance_management_reachability_verdict verdict)
{
    return verdict ==
               APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_CONSOLE_ALLOWED ||
           verdict ==
               APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED ||
           verdict ==
               APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_REMOTE_OPT_IN_ALLOWED;
}

const char *appliance_management_reachability_mode_string(
    enum appliance_management_reachability_mode mode)
{
    switch (mode) {
    case APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY:
        return "CONSOLE_ONLY";
    case APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE:
        return "MANAGEMENT_INTERFACE";
    case APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN:
        return "REMOTE_OPT_IN";
    default:
        return NULL;
    }
}

const char *appliance_management_reachability_verdict_string(
    enum appliance_management_reachability_verdict verdict)
{
    switch (verdict) {
    case APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_CONSOLE_ALLOWED:
        return "CONSOLE_ALLOWED";
    case APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED:
        return "MANAGEMENT_INTERFACE_ALLOWED";
    case APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_REMOTE_OPT_IN_ALLOWED:
        return "REMOTE_OPT_IN_ALLOWED";
    case APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_UNPUBLISHED:
        return "DENIED_UNPUBLISHED";
    case APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_CONSOLE_ONLY:
        return "DENIED_CONSOLE_ONLY";
    case APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE:
        return "DENIED_OUTER_INTERFACE";
    case APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NOT_ALLOWLISTED:
        return "DENIED_NOT_ALLOWLISTED";
    case APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NO_CLIENT_CERTIFICATE:
        return "DENIED_NO_CLIENT_CERTIFICATE";
    default:
        return NULL;
    }
}

const char *appliance_management_reachability_readiness_string(
    enum appliance_management_reachability_readiness readiness)
{
    switch (readiness) {
    case APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_READY:
        return "READY";
    default:
        return NULL;
    }
}

const char *appliance_management_reachability_status_string(
    enum appliance_management_reachability_status status)
{
    switch (status) {
    case APPLIANCE_MANAGEMENT_REACHABILITY_OK:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_OK";
    case APPLIANCE_MANAGEMENT_REACHABILITY_NULL:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_NULL";
    case APPLIANCE_MANAGEMENT_REACHABILITY_BAD_STATE:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_BAD_STATE";
    case APPLIANCE_MANAGEMENT_REACHABILITY_BAD_VERSION:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_BAD_VERSION";
    case APPLIANCE_MANAGEMENT_REACHABILITY_BAD_MODE:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_BAD_MODE";
    case APPLIANCE_MANAGEMENT_REACHABILITY_BAD_INTERFACE:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_BAD_INTERFACE";
    case APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ADDRESS:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ADDRESS";
    case APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST";
    case APPLIANCE_MANAGEMENT_REACHABILITY_ALREADY_PUBLISHED:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_ALREADY_PUBLISHED";
    case APPLIANCE_MANAGEMENT_REACHABILITY_UNPUBLISHED:
        return "APPLIANCE_MANAGEMENT_REACHABILITY_UNPUBLISHED";
    default:
        return NULL;
    }
}
