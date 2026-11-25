/*
 * ITEM-385 focused host proof for the management reachability policy: the
 * console manages the appliance with no network at all, the management plane is
 * reachable only on the declared management interface, and remote
 * administration requires an explicit opt-in with a source allowlist and,
 * when the policy demands it, a verified client certificate.
 */
#include "appliance_management_reachability.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_MANAGEMENT_REACHABILITY_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const uint8_t management_address[4] = { 10U, 0U, 0U, 5U };
static const uint8_t outer_address[4] = { 192U, 168U, 1U, 10U };
static const uint8_t admin_address[4] = { 203U, 0U, 113U, 7U };

static struct appliance_management_reachability_policy make_policy(
    enum appliance_management_reachability_mode mode)
{
    struct appliance_management_reachability_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = APPLIANCE_MANAGEMENT_REACHABILITY_VERSION;
    policy.mode = mode;
    policy.management_interface = 1U;
    memcpy(policy.management_address, management_address,
           APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH);
    return policy;
}

static struct appliance_management_reachability_peer make_peer(
    uint32_t interface_index,
    const uint8_t *address)
{
    struct appliance_management_reachability_peer peer;

    memset(&peer, 0, sizeof(peer));
    peer.interface_index = interface_index;
    memcpy(peer.source_address, address,
           APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH);
    return peer;
}

static void test_policy_validation(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;

    memset(&policy, 0, sizeof(policy));
    CHECK(appliance_management_reachability_policy_validate(&policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_BAD_VERSION,
          "policy without a version is rejected");
    CHECK(appliance_management_reachability_policy_validate(NULL) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_NULL,
          "null policy is rejected");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE);
    CHECK(appliance_management_reachability_policy_validate(&policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "management-interface policy validates");

    policy.mode = (enum appliance_management_reachability_mode)99;
    CHECK(appliance_management_reachability_policy_validate(&policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_BAD_MODE,
          "unknown mode is rejected");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN);
    policy.allowlist_count = APPLIANCE_MANAGEMENT_REACHABILITY_ALLOWLIST_MAX + 1U;
    CHECK(appliance_management_reachability_policy_validate(&policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST,
          "allowlist larger than the bound is rejected");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN);
    policy.allowlist_count = 2U;
    memcpy(policy.allowlist[0], admin_address,
           APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH);
    memcpy(policy.allowlist[1], admin_address,
           APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH);
    CHECK(appliance_management_reachability_policy_validate(&policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST,
          "duplicate allowlist entries are rejected");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE);
    policy.management_interface =
        APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_NONE;
    CHECK(appliance_management_reachability_policy_validate(&policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "a policy without a bound interface is valid but degraded");

    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "selection publishes");
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_ALREADY_PUBLISHED,
          "second publication is refused");
    CHECK(appliance_management_reachability_selection_init(NULL, &policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_NULL,
          "null selection is rejected");
}

static void test_readiness(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;

    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_readiness(&selection) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_UNAVAILABLE,
          "unpublished policy reports UNAVAILABLE");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE);
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "management-interface selection publishes");
    CHECK(appliance_management_reachability_readiness(&selection) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_READY,
          "bound management interface reports READY");

    policy.management_interface =
        APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_NONE;
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "degraded selection publishes");
    CHECK(appliance_management_reachability_readiness(&selection) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED,
          "missing management interface reports DEGRADED");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY);
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "console-only selection publishes");
    CHECK(appliance_management_reachability_readiness(&selection) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED,
          "console-only policy reports DEGRADED, not READY");
}

static void test_console_always_allowed(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;
    struct appliance_management_reachability_peer peer;
    enum appliance_management_reachability_verdict verdict;

    memset(&selection, 0, sizeof(selection));
    memset(&peer, 0, sizeof(peer));
    peer.console = true;
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "console evaluation succeeds without a policy");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_CONSOLE_ALLOWED,
          "console is allowed without any published policy");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY);
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "console-only selection publishes for the console case");
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "console evaluation succeeds in console-only mode");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_CONSOLE_ALLOWED,
          "console stays allowed in console-only mode");
}

static void test_network_denied_by_default(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;
    struct appliance_management_reachability_peer peer;
    enum appliance_management_reachability_verdict verdict;

    memset(&selection, 0, sizeof(selection));
    peer = make_peer(1U, management_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "evaluation succeeds without a published policy");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_UNPUBLISHED,
          "network request is denied while no policy is published");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY);
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "console-only policy publishes");
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "evaluation succeeds in console-only mode");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_CONSOLE_ONLY,
          "network request is denied in console-only mode");
}

static void test_management_interface_boundary(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;
    struct appliance_management_reachability_peer peer;
    enum appliance_management_reachability_verdict verdict;

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE);
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "management-interface policy publishes");

    peer = make_peer(1U, management_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "management-interface peer evaluates");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED,
          "request on the management interface is allowed");

    peer = make_peer(2U, outer_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "outer-interface peer evaluates");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE,
          "request on the outer interface is denied by default");

    peer = make_peer(1U, outer_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "wrong-address peer evaluates");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE,
          "wrong address on the management interface is denied");

    /* A wildcard address binds the interface only. */
    memset(policy.management_address, 0,
           APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH);
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "wildcard policy publishes");
    peer = make_peer(1U, admin_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "wildcard peer evaluates");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED,
          "wildcard address admits any address on the management interface");
    peer = make_peer(9U, admin_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "unknown-interface peer evaluates");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE,
          "unknown interface is denied even with a wildcard address");
}

static void test_remote_opt_in(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;
    struct appliance_management_reachability_peer peer;
    enum appliance_management_reachability_verdict verdict;

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN);
    policy.require_client_certificate = true;
    policy.allowlist_count = 0U;
    CHECK(appliance_management_reachability_policy_validate(&policy) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST,
          "remote opt-in without an explicit allowlist is refused");

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN);
    policy.require_client_certificate = true;
    policy.allowlist_count = 1U;
    memcpy(policy.allowlist[0], admin_address,
           APPLIANCE_MANAGEMENT_REACHABILITY_ADDRESS_LENGTH);
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "remote opt-in policy publishes");

    peer = make_peer(2U, admin_address);
    peer.client_certificate_present = true;
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "allowlisted remote peer with a certificate evaluates");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_REMOTE_OPT_IN_ALLOWED,
          "allowlisted remote peer with a certificate is admitted");

    peer = make_peer(2U, admin_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "allowlisted remote peer without a certificate evaluates");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NO_CLIENT_CERTIFICATE,
          "remote peer without the required certificate is denied");

    peer = make_peer(2U, outer_address);
    peer.client_certificate_present = true;
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "non-allowlisted remote peer evaluates");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NOT_ALLOWLISTED,
          "remote peer outside the allowlist is denied");

    policy.require_client_certificate = false;
    memset(&selection, 0, sizeof(selection));
    CHECK(appliance_management_reachability_selection_init(
              &selection, &policy) == APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "policy without a certificate requirement publishes");
    peer = make_peer(2U, admin_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "allowlisted peer evaluates without a certificate requirement");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_REMOTE_OPT_IN_ALLOWED,
          "allowlist alone admits when no certificate is required");

    /* The management interface stays admitted in remote opt-in mode. */
    peer = make_peer(1U, management_address);
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_OK,
          "management-interface peer evaluates in remote opt-in mode");
    CHECK(verdict ==
              APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED,
          "management interface remains admitted regardless of opt-in");
}

static void test_argument_guards(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;
    struct appliance_management_reachability_peer peer;
    enum appliance_management_reachability_verdict verdict;

    policy = make_policy(
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE);
    memset(&selection, 0, sizeof(selection));
    peer = make_peer(1U, management_address);
    CHECK(appliance_management_reachability_evaluate(
              NULL, &peer, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_NULL,
          "null selection is rejected");
    CHECK(appliance_management_reachability_evaluate(
              &selection, NULL, &verdict) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_NULL,
          "null peer is rejected");
    CHECK(appliance_management_reachability_evaluate(
              &selection, &peer, NULL) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_NULL,
          "null verdict is rejected");
    CHECK(appliance_management_reachability_readiness(NULL) ==
              APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_UNAVAILABLE,
          "null readiness reports UNAVAILABLE");

    CHECK(strcmp(appliance_management_reachability_mode_string(
                     APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN),
                 "REMOTE_OPT_IN") == 0,
          "mode string is stable");
    CHECK(strcmp(appliance_management_reachability_verdict_string(
                     APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE),
                 "DENIED_OUTER_INTERFACE") == 0,
          "verdict string is stable");
    CHECK(strcmp(appliance_management_reachability_readiness_string(
                     APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED),
                 "DEGRADED") == 0,
          "readiness string is stable");
    CHECK(strcmp(appliance_management_reachability_status_string(
                     APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST),
                 "APPLIANCE_MANAGEMENT_REACHABILITY_BAD_ALLOWLIST") == 0,
          "status string is stable");
}

int main(void)
{
    test_policy_validation();
    test_readiness();
    test_console_always_allowed();
    test_network_denied_by_default();
    test_management_interface_boundary();
    test_remote_opt_in();
    test_argument_guards();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance management reachability host tests: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("Appliance management reachability host tests: ALL OK\n");
    return 0;
}

#else
typedef int appliance_management_reachability_host_translation_unit_guard;
#endif
