#include "credential_verifier.h"

#include <stddef.h>
#include <stdint.h>

enum credential_verifier_status credential_verifier_validate_verifier(
    const struct credential_verifier *verifier)
{
    if (verifier == NULL) {
        return CREDENTIAL_VERIFIER_NULL;
    }
    if (verifier->version != CREDENTIAL_VERIFIER_VERSION) {
        return CREDENTIAL_VERIFIER_INVALID_VERSION;
    }
    if (verifier->principal_id < CREDENTIAL_VERIFIER_ID_MIN ||
        verifier->principal_id > CREDENTIAL_VERIFIER_ID_MAX) {
        return CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID;
    }
    if (verifier->state != CREDENTIAL_VERIFIER_STATE_DISABLED &&
        verifier->state != CREDENTIAL_VERIFIER_STATE_ENABLED) {
        return CREDENTIAL_VERIFIER_INVALID_STATE;
    }
    return CREDENTIAL_VERIFIER_OK;
}

enum credential_verifier_status credential_verifier_validate_input(
    const struct credential_input *input)
{
    if (input == NULL) {
        return CREDENTIAL_VERIFIER_NULL;
    }
    if (input->version != CREDENTIAL_VERIFIER_VERSION) {
        return CREDENTIAL_VERIFIER_INVALID_VERSION;
    }
    if (input->principal_id < CREDENTIAL_VERIFIER_ID_MIN ||
        input->principal_id > CREDENTIAL_VERIFIER_ID_MAX) {
        return CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID;
    }
    return CREDENTIAL_VERIFIER_OK;
}

enum credential_verifier_status credential_verifier_verify(
    const struct credential_verifier *verifier,
    const struct credential_input *input)
{
    enum credential_verifier_status status;
    volatile uint8_t mismatch = 0U;
    uint32_t index;

    if (verifier == NULL || input == NULL) {
        return CREDENTIAL_VERIFIER_NULL;
    }
    status = credential_verifier_validate_verifier(verifier);
    if (status != CREDENTIAL_VERIFIER_OK) {
        return status;
    }
    status = credential_verifier_validate_input(input);
    if (status != CREDENTIAL_VERIFIER_OK) {
        return status;
    }
    if (verifier->state == CREDENTIAL_VERIFIER_STATE_DISABLED) {
        return CREDENTIAL_VERIFIER_DISABLED;
    }
    if (verifier->principal_id != input->principal_id) {
        return CREDENTIAL_VERIFIER_PRINCIPAL_MISMATCH;
    }
    for (index = 0U; index < CREDENTIAL_VERIFIER_SECRET_LENGTH; ++index) {
        mismatch |= (uint8_t)(verifier->expected_secret[index] ^
                              input->secret[index]);
    }
    if (mismatch != 0U) {
        return CREDENTIAL_VERIFIER_AUTHENTICATION_FAILED;
    }
    return CREDENTIAL_VERIFIER_OK;
}

const char *credential_verifier_status_string(
    enum credential_verifier_status status)
{
    switch (status) {
    case CREDENTIAL_VERIFIER_OK:
        return "CREDENTIAL_VERIFIER_OK";
    case CREDENTIAL_VERIFIER_NULL:
        return "CREDENTIAL_VERIFIER_NULL";
    case CREDENTIAL_VERIFIER_INVALID_VERSION:
        return "CREDENTIAL_VERIFIER_INVALID_VERSION";
    case CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID:
        return "CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID";
    case CREDENTIAL_VERIFIER_INVALID_STATE:
        return "CREDENTIAL_VERIFIER_INVALID_STATE";
    case CREDENTIAL_VERIFIER_DISABLED:
        return "CREDENTIAL_VERIFIER_DISABLED";
    case CREDENTIAL_VERIFIER_PRINCIPAL_MISMATCH:
        return "CREDENTIAL_VERIFIER_PRINCIPAL_MISMATCH";
    case CREDENTIAL_VERIFIER_AUTHENTICATION_FAILED:
        return "CREDENTIAL_VERIFIER_AUTHENTICATION_FAILED";
    default:
        return NULL;
    }
}
