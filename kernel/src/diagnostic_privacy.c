#include "diagnostic_privacy.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *const diagnostic_privacy_secret_keys[
    DIAGNOSTIC_PRIVACY_SECRET_KEY_COUNT] = {
    "credential",
    "secret",
    "password",
    "token",
    "private_key",
    "capability",
};

static void diagnostic_privacy_zero_bytes(
    unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

enum diagnostic_privacy_status diagnostic_privacy_classify_key(
    const void *key,
    size_t key_length,
    enum diagnostic_privacy_category *category_out)
{
    size_t index;

    if (key_length > 0U && key == NULL) {
        return DIAGNOSTIC_PRIVACY_NULL;
    }
    if (category_out == NULL) {
        return DIAGNOSTIC_PRIVACY_NULL;
    }

    *category_out = DIAGNOSTIC_PRIVACY_CATEGORY_NONE;
    for (index = 0U;
         index < DIAGNOSTIC_PRIVACY_SECRET_KEY_COUNT;
         ++index) {
        const char *secret_key =
            diagnostic_privacy_secret_keys[index];
        const size_t secret_length = strlen(secret_key);

        if (key_length == secret_length &&
            memcmp(key, secret_key, secret_length) == 0) {
            *category_out =
                DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL;
            return DIAGNOSTIC_PRIVACY_OK;
        }
    }
    return DIAGNOSTIC_PRIVACY_OK;
}

enum diagnostic_privacy_status diagnostic_privacy_classify_verifier(
    const struct credential_verifier *verifier,
    enum diagnostic_privacy_category *category_out)
{
    if (verifier == NULL || category_out == NULL) {
        return DIAGNOSTIC_PRIVACY_NULL;
    }
    *category_out =
        DIAGNOSTIC_PRIVACY_CATEGORY_VERIFIER_MATERIAL;
    return DIAGNOSTIC_PRIVACY_OK;
}

enum diagnostic_privacy_status diagnostic_privacy_classify_capability(
    const struct capability *capability,
    enum diagnostic_privacy_category *category_out)
{
    if (capability == NULL || category_out == NULL) {
        return DIAGNOSTIC_PRIVACY_NULL;
    }
    *category_out =
        DIAGNOSTIC_PRIVACY_CATEGORY_CAPABILITY_VALUE;
    return DIAGNOSTIC_PRIVACY_OK;
}

enum diagnostic_privacy_status diagnostic_privacy_classify_address(
    enum diagnostic_privacy_address_kind kind,
    const void *data,
    size_t length,
    enum diagnostic_privacy_category *category_out)
{
    size_t expected_length;

    if (category_out == NULL) {
        return DIAGNOSTIC_PRIVACY_NULL;
    }

    switch (kind) {
    case DIAGNOSTIC_PRIVACY_ADDRESS_IPV4:
        expected_length = DIAGNOSTIC_PRIVACY_IPV4_LENGTH;
        break;
    case DIAGNOSTIC_PRIVACY_ADDRESS_MAC:
        expected_length = DIAGNOSTIC_PRIVACY_MAC_LENGTH;
        break;
    case DIAGNOSTIC_PRIVACY_ADDRESS_BUS:
        expected_length = DIAGNOSTIC_PRIVACY_BUS_ADDRESS_LENGTH;
        break;
    default:
        return DIAGNOSTIC_PRIVACY_BAD_ADDRESS_KIND;
    }

    if (data == NULL) {
        return DIAGNOSTIC_PRIVACY_NULL;
    }
    if (length != expected_length) {
        return DIAGNOSTIC_PRIVACY_BAD_LENGTH;
    }

    *category_out =
        DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS;
    return DIAGNOSTIC_PRIVACY_OK;
}

enum diagnostic_privacy_status diagnostic_privacy_redact(
    enum diagnostic_privacy_category category,
    const void *input,
    size_t input_length,
    void *output,
    size_t capacity,
    size_t *output_length)
{
    unsigned char *destination = output;

    if ((input_length > 0U && input == NULL) ||
        output == NULL ||
        output_length == NULL) {
        return DIAGNOSTIC_PRIVACY_NULL;
    }
    if (category >= DIAGNOSTIC_PRIVACY_CATEGORY_COUNT) {
        return DIAGNOSTIC_PRIVACY_BAD_CATEGORY;
    }
    if (capacity < input_length) {
        return DIAGNOSTIC_PRIVACY_TOO_SMALL;
    }

    if (category == DIAGNOSTIC_PRIVACY_CATEGORY_NONE) {
        if (input_length > 0U) {
            memcpy(destination, input, input_length);
        }
    } else {
        diagnostic_privacy_zero_bytes(
            destination, input_length);
    }
    *output_length = input_length;
    return DIAGNOSTIC_PRIVACY_OK;
}

const char *diagnostic_privacy_category_string(
    enum diagnostic_privacy_category category)
{
    switch (category) {
    case DIAGNOSTIC_PRIVACY_CATEGORY_NONE:
        return "DIAGNOSTIC_PRIVACY_CATEGORY_NONE";
    case DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL:
        return "DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL";
    case DIAGNOSTIC_PRIVACY_CATEGORY_VERIFIER_MATERIAL:
        return "DIAGNOSTIC_PRIVACY_CATEGORY_VERIFIER_MATERIAL";
    case DIAGNOSTIC_PRIVACY_CATEGORY_CAPABILITY_VALUE:
        return "DIAGNOSTIC_PRIVACY_CATEGORY_CAPABILITY_VALUE";
    case DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS:
        return "DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS";
    default:
        return "DIAGNOSTIC_PRIVACY_CATEGORY_UNKNOWN";
    }
}

const char *diagnostic_privacy_status_string(
    enum diagnostic_privacy_status status)
{
    switch (status) {
    case DIAGNOSTIC_PRIVACY_OK:
        return "DIAGNOSTIC_PRIVACY_OK";
    case DIAGNOSTIC_PRIVACY_NULL:
        return "DIAGNOSTIC_PRIVACY_NULL";
    case DIAGNOSTIC_PRIVACY_BAD_CATEGORY:
        return "DIAGNOSTIC_PRIVACY_BAD_CATEGORY";
    case DIAGNOSTIC_PRIVACY_BAD_ADDRESS_KIND:
        return "DIAGNOSTIC_PRIVACY_BAD_ADDRESS_KIND";
    case DIAGNOSTIC_PRIVACY_BAD_LENGTH:
        return "DIAGNOSTIC_PRIVACY_BAD_LENGTH";
    case DIAGNOSTIC_PRIVACY_TOO_SMALL:
        return "DIAGNOSTIC_PRIVACY_TOO_SMALL";
    default:
        return "DIAGNOSTIC_PRIVACY_UNKNOWN";
    }
}

uint32_t diagnostic_privacy_version(void)
{
    return DIAGNOSTIC_PRIVACY_VERSION;
}

const char *diagnostic_privacy_policy_id(void)
{
    return DIAGNOSTIC_PRIVACY_POLICY_ID;
}
