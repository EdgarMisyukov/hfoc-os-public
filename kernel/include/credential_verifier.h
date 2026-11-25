#ifndef HFOC_CREDENTIAL_VERIFIER_H
#define HFOC_CREDENTIAL_VERIFIER_H

#include <stdint.h>

#define CREDENTIAL_VERIFIER_VERSION 1U
#define CREDENTIAL_VERIFIER_ID_MIN 1U
#define CREDENTIAL_VERIFIER_ID_MAX 255U
#define CREDENTIAL_VERIFIER_SECRET_LENGTH 32U

enum credential_verifier_state {
    CREDENTIAL_VERIFIER_STATE_DISABLED = 0,
    CREDENTIAL_VERIFIER_STATE_ENABLED = 1,
};

enum credential_verifier_status {
    CREDENTIAL_VERIFIER_OK = 0,
    CREDENTIAL_VERIFIER_NULL = 1,
    CREDENTIAL_VERIFIER_INVALID_VERSION = 2,
    CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID = 3,
    CREDENTIAL_VERIFIER_INVALID_STATE = 4,
    CREDENTIAL_VERIFIER_DISABLED = 5,
    CREDENTIAL_VERIFIER_PRINCIPAL_MISMATCH = 6,
    CREDENTIAL_VERIFIER_AUTHENTICATION_FAILED = 7,
};

struct credential_input {
    uint32_t version;
    uint32_t principal_id;
    uint8_t secret[CREDENTIAL_VERIFIER_SECRET_LENGTH];
};

struct credential_verifier {
    uint32_t version;
    uint32_t principal_id;
    enum credential_verifier_state state;
    uint8_t expected_secret[CREDENTIAL_VERIFIER_SECRET_LENGTH];
};

enum credential_verifier_status credential_verifier_validate_verifier(
    const struct credential_verifier *verifier);
enum credential_verifier_status credential_verifier_validate_input(
    const struct credential_input *input);
enum credential_verifier_status credential_verifier_verify(
    const struct credential_verifier *verifier,
    const struct credential_input *input);
const char *credential_verifier_status_string(
    enum credential_verifier_status status);

#endif
