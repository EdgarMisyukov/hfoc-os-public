#include "appliance_acme_account.h"

/*
 * ITEM-508 increment 24: the one binding point between the persisted appliance
 * identity and the ACME account key.  The module is transport-free and does
 * not call the certificate owner: it reads the prepared `key_der` the owner
 * publishes, so the only dependency is the header that defines that view.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_ACME_ACCOUNT_HOST_TEST)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char appliance_acme_account_record_prefix[] = "thumbprint=";
static char appliance_acme_account_record_buffer[
    APPLIANCE_ACME_ACCOUNT_RECORD_MAX];
static bool appliance_acme_account_record_ready;

static void appliance_acme_account_record_set(
    const struct appliance_acme_key *key)
{
    const char *thumbprint = appliance_acme_key_thumbprint(key);
    size_t thumbprint_length = 0U;
    size_t position = 0U;
    size_t index;

    if (thumbprint == NULL) {
        appliance_acme_account_record_ready = false;
        return;
    }
    while (thumbprint[thumbprint_length] != '\0') {
        ++thumbprint_length;
    }
    /*
     * The record is one bounded line: a thumbprint that would not fit is a
     * refusal, never a truncated record that names half an account.
     */
    if (sizeof(appliance_acme_account_record_prefix) - 1U +
            thumbprint_length + 1U >
        sizeof(appliance_acme_account_record_buffer)) {
        appliance_acme_account_record_ready = false;
        return;
    }
    for (index = 0U;
         index < sizeof(appliance_acme_account_record_prefix) - 1U;
         ++index) {
        appliance_acme_account_record_buffer[position++] =
            appliance_acme_account_record_prefix[index];
    }
    for (index = 0U; index < thumbprint_length; ++index) {
        appliance_acme_account_record_buffer[position++] = thumbprint[index];
    }
    appliance_acme_account_record_buffer[position] = '\0';
    appliance_acme_account_record_ready = true;
}

enum appliance_acme_account_status appliance_acme_account_bind(
    const struct appliance_production_certificate *identity,
    struct appliance_acme_key *key)
{
    enum appliance_acme_jws_status status;

    if (identity == NULL || key == NULL) {
        return APPLIANCE_ACME_ACCOUNT_NULL;
    }
    /*
     * A refusal invalidates the previous record: the caller must never read a
     * thumbprint that belongs to a key this bind did not accept.
     */
    appliance_acme_account_record_ready = false;
    if (!identity->initialized || !identity->available ||
        identity->key_length == 0U) {
        return APPLIANCE_ACME_ACCOUNT_IDENTITY_UNAVAILABLE;
    }
    if (identity->key_length >
        (uint32_t)APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX) {
        return APPLIANCE_ACME_ACCOUNT_KEY_UNUSABLE;
    }
    status = appliance_acme_key_import(key, identity->key_der,
                                       identity->key_length);
    if (status != APPLIANCE_ACME_JWS_OK) {
        return APPLIANCE_ACME_ACCOUNT_KEY_REFUSED;
    }
    appliance_acme_account_record_set(key);
    return APPLIANCE_ACME_ACCOUNT_OK;
}

const char *appliance_acme_account_record(void)
{
    if (!appliance_acme_account_record_ready) {
        return "unbound";
    }
    return appliance_acme_account_record_buffer;
}

const char *appliance_acme_account_status_string(
    enum appliance_acme_account_status status)
{
    switch (status) {
    case APPLIANCE_ACME_ACCOUNT_OK:
        return "OK";
    case APPLIANCE_ACME_ACCOUNT_NULL:
        return "NULL";
    case APPLIANCE_ACME_ACCOUNT_IDENTITY_UNAVAILABLE:
        return "IDENTITY_UNAVAILABLE";
    case APPLIANCE_ACME_ACCOUNT_KEY_UNUSABLE:
        return "KEY_UNUSABLE";
    case APPLIANCE_ACME_ACCOUNT_KEY_REFUSED:
        return "KEY_REFUSED";
    default:
        break;
    }
    return "UNKNOWN";
}

#endif
