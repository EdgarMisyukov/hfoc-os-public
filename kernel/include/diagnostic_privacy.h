#ifndef HFOC_DIAGNOSTIC_PRIVACY_H
#define HFOC_DIAGNOSTIC_PRIVACY_H

#include "capability.h"
#include "credential_verifier.h"

#include <stddef.h>
#include <stdint.h>

#define DIAGNOSTIC_PRIVACY_VERSION 1U
#define DIAGNOSTIC_PRIVACY_POLICY_ID "HFOC-DIAG-PRIV-1"
#define DIAGNOSTIC_PRIVACY_POLICY_ID_LENGTH 16U
#define DIAGNOSTIC_PRIVACY_SECRET_KEY_COUNT 6U
#define DIAGNOSTIC_PRIVACY_IPV4_LENGTH 4U
#define DIAGNOSTIC_PRIVACY_MAC_LENGTH 6U
#define DIAGNOSTIC_PRIVACY_BUS_ADDRESS_LENGTH 4U

enum diagnostic_privacy_category {
    DIAGNOSTIC_PRIVACY_CATEGORY_NONE = 0,
    DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL = 1,
    DIAGNOSTIC_PRIVACY_CATEGORY_VERIFIER_MATERIAL = 2,
    DIAGNOSTIC_PRIVACY_CATEGORY_CAPABILITY_VALUE = 3,
    DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS = 4,
    DIAGNOSTIC_PRIVACY_CATEGORY_COUNT = 5,
};

enum diagnostic_privacy_address_kind {
    DIAGNOSTIC_PRIVACY_ADDRESS_NONE = 0,
    DIAGNOSTIC_PRIVACY_ADDRESS_IPV4 = 1,
    DIAGNOSTIC_PRIVACY_ADDRESS_MAC = 2,
    DIAGNOSTIC_PRIVACY_ADDRESS_BUS = 3,
    DIAGNOSTIC_PRIVACY_ADDRESS_COUNT = 4,
};

enum diagnostic_privacy_status {
    DIAGNOSTIC_PRIVACY_OK = 0,
    DIAGNOSTIC_PRIVACY_NULL = 1,
    DIAGNOSTIC_PRIVACY_BAD_CATEGORY = 2,
    DIAGNOSTIC_PRIVACY_BAD_ADDRESS_KIND = 3,
    DIAGNOSTIC_PRIVACY_BAD_LENGTH = 4,
    DIAGNOSTIC_PRIVACY_TOO_SMALL = 5,
};

enum diagnostic_privacy_status diagnostic_privacy_classify_key(
    const void *key,
    size_t key_length,
    enum diagnostic_privacy_category *category_out);
enum diagnostic_privacy_status diagnostic_privacy_classify_verifier(
    const struct credential_verifier *verifier,
    enum diagnostic_privacy_category *category_out);
enum diagnostic_privacy_status diagnostic_privacy_classify_capability(
    const struct capability *capability,
    enum diagnostic_privacy_category *category_out);
enum diagnostic_privacy_status diagnostic_privacy_classify_address(
    enum diagnostic_privacy_address_kind kind,
    const void *data,
    size_t length,
    enum diagnostic_privacy_category *category_out);
enum diagnostic_privacy_status diagnostic_privacy_redact(
    enum diagnostic_privacy_category category,
    const void *input,
    size_t input_length,
    void *output,
    size_t capacity,
    size_t *output_length);
const char *diagnostic_privacy_category_string(
    enum diagnostic_privacy_category category);
const char *diagnostic_privacy_status_string(
    enum diagnostic_privacy_status status);
uint32_t diagnostic_privacy_version(void);
const char *diagnostic_privacy_policy_id(void);

#endif
