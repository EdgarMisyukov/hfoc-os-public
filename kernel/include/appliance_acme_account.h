#ifndef HFOC_APPLIANCE_ACME_ACCOUNT_H
#define HFOC_APPLIANCE_ACME_ACCOUNT_H

#include "appliance_acme_jws.h"
#include "appliance_production_certificate.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 24: the ACME account key is the persisted appliance
 * identity key.
 *
 * An ACME account is named by the public key its requests are signed with, and
 * the CA remembers that name.  If the key changed on every boot the appliance
 * would present a new account each time and could never own an order or a
 * renewal, so the account key must be the one that already survives a reboot:
 * the P-256 identity key the certificate owner generated and persisted in the
 * CERTIFICATE store (ITEM-386, record v2).  This module is the one binding
 * point: it takes the prepared identity, hands `key_der` to the JWS module
 * (which owns the private context and never exposes it) and publishes a
 * bounded record naming the public RFC 7638 thumbprint - the account's stable
 * name at the CA - never a key byte.
 *
 * The choice is deliberate and stated: the account key and the certificate key
 * are the same persisted identity key.  RFC 8555 permits it, and it is what
 * makes "the account survives a reboot" true without a second secret store;
 * a separate account key would be a later decision, not a silent one.
 *
 * `bind` refuses an identity that is not available, a DER outside the
 * certificate record's own bound, and a key the JWS module rejects (anything
 * that is not an EC P-256 key).  A refused bind leaves the record "unbound";
 * it never half-binds.
 */

#define APPLIANCE_ACME_ACCOUNT_VERSION 1U
/* "thumbprint=" plus the 43-character RFC 7638 value and a terminator. */
#define APPLIANCE_ACME_ACCOUNT_RECORD_MAX 96U

enum appliance_acme_account_status {
    APPLIANCE_ACME_ACCOUNT_OK = 0,
    APPLIANCE_ACME_ACCOUNT_NULL = 1,
    APPLIANCE_ACME_ACCOUNT_IDENTITY_UNAVAILABLE = 2,
    APPLIANCE_ACME_ACCOUNT_KEY_UNUSABLE = 3,
    APPLIANCE_ACME_ACCOUNT_KEY_REFUSED = 4,
};

/*
 * Bind the account key from the prepared identity.  On OK the key carries the
 * public JWK and thumbprint; on any refusal it is left zeroed.
 */
enum appliance_acme_account_status appliance_acme_account_bind(
    const struct appliance_production_certificate *identity,
    struct appliance_acme_key *key);

/* The bounded record of the last successful bind, or "unbound". */
const char *appliance_acme_account_record(void);

const char *appliance_acme_account_status_string(
    enum appliance_acme_account_status status);

#endif
