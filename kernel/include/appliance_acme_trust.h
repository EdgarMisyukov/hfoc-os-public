#ifndef HFOC_APPLIANCE_ACME_TRUST_H
#define HFOC_APPLIANCE_ACME_TRUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 2: the trust the ACME client role is allowed to have.
 *
 * The appliance verifies the ACME server against exactly one root, named in
 * the image and updated with the OS version; there is no network root store,
 * no "system" trust and no way for a reply to introduce an anchor.  The root
 * is data of this module (`appliance_acme_trust_anchor_*`), and the module is
 * the only place the client-role certificate chain is verified.
 *
 * Fail-closed, in this order:
 *
 *   * a call without a validated wall clock is refused with `BAD_CLOCK`
 *     before anything is parsed: the item does not start an order on an
 *     unvalidated clock (ITEM-374 validates it through SNTP);
 *   * the hostname is validated as an exact DNS name first - no wildcard, no
 *     trailing dot, no empty label - because a malformed name can never be
 *     "verified" and must not read as a chain problem;
 *   * the presented chain is parsed from DER and bounded; a chain that does
 *     not parse is a refusal, never a partial success;
 *   * every member's validity window is walked against the very instant the
 *     caller hands in.  mbedTLS runs its own date checks only when the
 *     profile carries a wall clock (`MBEDTLS_HAVE_TIME_DATE`), and the frozen
 *     profile deliberately does not, so an expired issuer would otherwise be
 *     accepted with no flag set (the same walk ITEM-503's import gate does);
 *   * the chain is verified against the named anchor and the name is verified
 *     against the leaf, and any verification flag at all refuses the call.
 *
 * A refusal is counted and its reason is readable (`last_status`, and the
 * mbedTLS flags in `last_flags` for the records), because "why did the
 * appliance not trust the ACME server" has to be debuggable from a record
 * rather than from a silent failure.
 */

#define APPLIANCE_ACME_TRUST_VERSION 1U
/* One exact DNS name, the same bound the certificate-name table uses. */
#define APPLIANCE_ACME_TRUST_NAME_MAX 253U
/* The whole presented chain, leaf first: bounded so a reply cannot grow it. */
#define APPLIANCE_ACME_TRUST_CHAIN_MAX 8192U
#define APPLIANCE_ACME_TRUST_CHAIN_MEMBERS_MAX 8U

enum appliance_acme_trust_status {
    APPLIANCE_ACME_TRUST_OK = 0,
    APPLIANCE_ACME_TRUST_NULL = 1,
    APPLIANCE_ACME_TRUST_BAD_STATE = 2,
    /* No validated wall clock: the item starts no order. */
    APPLIANCE_ACME_TRUST_BAD_CLOCK = 3,
    /* The presented name is not an exact DNS name. */
    APPLIANCE_ACME_TRUST_BAD_NAME = 4,
    /* The named anchor is missing or does not parse. */
    APPLIANCE_ACME_TRUST_ANCHOR_UNAVAILABLE = 5,
    /* The presented chain does not parse, or is empty or over the bound. */
    APPLIANCE_ACME_TRUST_CHAIN_PARSE_FAILED = 6,
    /* A member is expired or not yet valid at the instant handed in. */
    APPLIANCE_ACME_TRUST_CHAIN_WINDOW_REJECTED = 7,
    /* The chain does not verify against the named anchor. */
    APPLIANCE_ACME_TRUST_CHAIN_REJECTED = 8,
    /* The chain verifies, but not for this name. */
    APPLIANCE_ACME_TRUST_NAME_REJECTED = 9,
};

struct appliance_acme_trust {
    uint32_t version;
    bool initialized;
    /* Borrowed, never copied: the anchor is the image's own data. */
    const unsigned char *anchor_der;
    size_t anchor_length;
    uint32_t verify_count;
    uint32_t refusal_count;
    /* mbedTLS flags of the last chain verification, for the record. */
    uint32_t last_flags;
    enum appliance_acme_trust_status last_status;
    /*
     * ITEM-508 increment 35: the leaf's own `notAfter`, in epoch seconds, as
     * read by the same window walk that accepted it.  Zero means the last call
     * did not accept a chain (a refusal, or no call yet), so a caller that
     * feeds the renewal policy never moves an expiry from a chain that did not
     * verify.
     */
    uint32_t leaf_not_after_epoch_seconds;
};

/* The image's named anchor: ISRG Root X1 as DER, and its length. */
const unsigned char *appliance_acme_trust_anchor_der_data(void);
size_t appliance_acme_trust_anchor_der_length(void);

/*
 * Bind an anchor.  The caller passes the image's own data in production and a
 * fixture anchor in a host proof, so every refusal path can be driven without
 * a network.  An empty or unparseable anchor is refused by `verify`, not here:
 * `init` only records what it was given.
 */
enum appliance_acme_trust_status appliance_acme_trust_init(
    struct appliance_acme_trust *trust,
    const unsigned char *anchor_der,
    size_t anchor_length);

/*
 * Verify one presented chain (leaf first, DER, optionally concatenated) for
 * one exact name at one validated instant.  The anchor is parsed once and
 * cached; a later call with a different anchor re-parses it.
 */
enum appliance_acme_trust_status appliance_acme_trust_verify(
    struct appliance_acme_trust *trust,
    const unsigned char *chain_der,
    size_t chain_length,
    const char *hostname,
    size_t hostname_length,
    uint32_t now_epoch_seconds);

/*
 * The verified leaf's `notAfter` in epoch seconds, or zero when the last
 * verification did not accept a chain.  The order path uses it as the one
 * expiry the renewal policy may move to.
 */
uint32_t appliance_acme_trust_leaf_not_after(
    const struct appliance_acme_trust *trust);

const char *appliance_acme_trust_status_string(
    enum appliance_acme_trust_status status);

#endif
