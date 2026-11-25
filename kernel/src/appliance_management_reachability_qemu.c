#ifdef HFOC_APPLIANCE_MANAGEMENT_PLANE_QEMU_TEST

/*
 * ITEM-385 management reachability proof, run inside a real QEMU boot before
 * the HTTPS listener accepts its first connection.
 *
 * The appliance has one network interface today, so the QEMU proof exercises
 * the policy with declared peers (interface index plus source address) and the
 * live wire gate proves the same evaluator is consulted for a real peer on a
 * real TLS connection. Every branch the operator depends on is asserted here:
 * the console keeps working with no network and with no published policy, the
 * outer interface is denied by default, the management interface is served,
 * remote administration needs an explicit opt-in, and readiness stays honest.
 */
#include "appliance_management_reachability.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MGMT_REACH_MANAGEMENT_INTERFACE 1U
#define MGMT_REACH_OUTER_INTERFACE 0U

static void mgmt_reach_fail(const char *message)
    __attribute__((noreturn));

static void mgmt_reach_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[MGMT-REACH] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void mgmt_reach_require(bool condition, const char *message)
{
    if (!condition) {
        mgmt_reach_fail(message);
    }
}

static void mgmt_reach_mark(const char *marker)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(marker);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
}

static void mgmt_reach_policy(
    struct appliance_management_reachability_policy *policy,
    enum appliance_management_reachability_mode mode)
{
    memset(policy, 0, sizeof(*policy));
    policy->version = APPLIANCE_MANAGEMENT_REACHABILITY_VERSION;
    policy->mode = mode;
    policy->management_interface = MGMT_REACH_MANAGEMENT_INTERFACE;
    policy->management_address[0] = 10U;
    policy->management_address[3] = 5U;
}

static void mgmt_reach_peer(
    struct appliance_management_reachability_peer *peer,
    uint32_t interface_index,
    uint8_t last_octet,
    bool client_certificate_present)
{
    memset(peer, 0, sizeof(*peer));
    peer->interface_index = interface_index;
    peer->source_address[0] = 10U;
    peer->source_address[3] = last_octet;
    peer->client_certificate_present = client_certificate_present;
}

static void mgmt_reach_expect(
    const struct appliance_management_reachability_selection *selection,
    const struct appliance_management_reachability_peer *peer,
    enum appliance_management_reachability_verdict expected,
    const char *message)
{
    enum appliance_management_reachability_verdict verdict =
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_UNPUBLISHED;

    mgmt_reach_require(
        appliance_management_reachability_evaluate(
            selection, peer, &verdict) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_OK,
        "reachability evaluation");
    mgmt_reach_require(verdict == expected, message);
}

static void mgmt_reach_proof_console(void)
{
    struct appliance_management_reachability_selection selection;
    struct appliance_management_reachability_peer peer;

    /* No policy is published yet: the console is still the break-glass path. */
    memset(&selection, 0, sizeof(selection));
    memset(&peer, 0, sizeof(peer));
    peer.console = true;
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_CONSOLE_ALLOWED,
        "console allowed without a published policy");
    mgmt_reach_require(
        appliance_management_reachability_readiness(&selection) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_UNAVAILABLE,
        "unpublished readiness");

    /* A network peer before publication is refused. */
    mgmt_reach_peer(&peer, MGMT_REACH_MANAGEMENT_INTERFACE, 5U, false);
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_UNPUBLISHED,
        "network peer denied without a published policy");
    mgmt_reach_mark("[MGMT-REACH] console allowed ok");
}

static void mgmt_reach_proof_interface(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;
    struct appliance_management_reachability_peer peer;

    mgmt_reach_policy(
        &policy,
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE);
    memset(&selection, 0, sizeof(selection));
    mgmt_reach_require(
        appliance_management_reachability_selection_init(
            &selection, &policy) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_OK,
        "management policy publication");
    mgmt_reach_require(
        appliance_management_reachability_readiness(&selection) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_READY,
        "management interface readiness");
    mgmt_reach_require(
        appliance_management_reachability_selection_init(
            &selection, &policy) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_ALREADY_PUBLISHED,
        "second publication refused");

    mgmt_reach_peer(&peer, MGMT_REACH_MANAGEMENT_INTERFACE, 5U, false);
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED,
        "management interface peer served");
    mgmt_reach_mark("[MGMT-REACH] management interface served ok");

    mgmt_reach_peer(&peer, MGMT_REACH_OUTER_INTERFACE, 5U, false);
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE,
        "outer interface peer denied");
    mgmt_reach_mark("[MGMT-REACH] outer interface denied ok");

    mgmt_reach_peer(&peer, MGMT_REACH_MANAGEMENT_INTERFACE, 9U, false);
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE,
        "unknown address on the management interface denied");
}

static void mgmt_reach_proof_remote_opt_in(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;
    struct appliance_management_reachability_peer peer;

    mgmt_reach_policy(
        &policy,
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN);
    policy.require_client_certificate = true;
    policy.allowlist_count = 1U;
    policy.allowlist[0][0] = 203U;
    policy.allowlist[0][1] = 0U;
    policy.allowlist[0][2] = 113U;
    policy.allowlist[0][3] = 7U;
    memset(&selection, 0, sizeof(selection));
    mgmt_reach_require(
        appliance_management_reachability_selection_init(
            &selection, &policy) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_OK,
        "remote opt-in publication");

    memset(&peer, 0, sizeof(peer));
    peer.interface_index = MGMT_REACH_OUTER_INTERFACE;
    peer.source_address[0] = 203U;
    peer.source_address[2] = 113U;
    peer.source_address[3] = 7U;
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NO_CLIENT_CERTIFICATE,
        "allowlisted remote peer without a certificate denied");
    mgmt_reach_mark("[MGMT-REACH] client certificate denied ok");

    peer.client_certificate_present = true;
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_REMOTE_OPT_IN_ALLOWED,
        "allowlisted remote peer with a certificate served");
    mgmt_reach_mark("[MGMT-REACH] remote opt-in admitted ok");

    peer.source_address[3] = 8U;
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NOT_ALLOWLISTED,
        "remote peer outside the allowlist denied");
    mgmt_reach_mark("[MGMT-REACH] remote opt-in denied ok");

    /* The management interface stays admitted regardless of the opt-in. */
    mgmt_reach_peer(&peer, MGMT_REACH_MANAGEMENT_INTERFACE, 5U, false);
    mgmt_reach_expect(
        &selection,
        &peer,
        APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_MANAGEMENT_INTERFACE_ALLOWED,
        "management interface admitted in remote opt-in mode");
}

static void mgmt_reach_proof_readiness(void)
{
    struct appliance_management_reachability_policy policy;
    struct appliance_management_reachability_selection selection;

    mgmt_reach_policy(
        &policy,
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY);
    memset(&selection, 0, sizeof(selection));
    mgmt_reach_require(
        appliance_management_reachability_selection_init(
            &selection, &policy) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_OK,
        "console-only publication");
    mgmt_reach_require(
        appliance_management_reachability_readiness(&selection) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED,
        "console-only readiness stays honest");
    mgmt_reach_mark("[MGMT-REACH] readiness degraded ok");

    mgmt_reach_policy(
        &policy,
        APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE);
    policy.management_interface =
        APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_NONE;
    memset(&selection, 0, sizeof(selection));
    mgmt_reach_require(
        appliance_management_reachability_selection_init(
            &selection, &policy) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_OK,
        "unbound interface publication");
    mgmt_reach_require(
        appliance_management_reachability_readiness(&selection) ==
            APPLIANCE_MANAGEMENT_REACHABILITY_READINESS_DEGRADED,
        "unbound management interface reports DEGRADED");
}

void appliance_management_reachability_qemu_test_init(void)
{
    mgmt_reach_mark("[MGMT-REACH] proof init");
    mgmt_reach_proof_console();
    mgmt_reach_proof_interface();
    mgmt_reach_proof_remote_opt_in();
    mgmt_reach_proof_readiness();
    mgmt_reach_mark("[MGMT-REACH] proof OK");
}

#else
typedef int appliance_management_reachability_qemu_translation_unit_guard;
#endif
