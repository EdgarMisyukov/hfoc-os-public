#ifndef HFOC_APPLIANCE_SECRET_PROTECTION_H
#define HFOC_APPLIANCE_SECRET_PROTECTION_H

#include "sha256.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_SECRET_PROTECTION_VERSION 1U
#define APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH 32U
#define APPLIANCE_SECRET_PROTECTION_TOKEN_LENGTH 32U
#define APPLIANCE_SECRET_PROTECTION_MAX_SECRET_LENGTH 1024U
#define APPLIANCE_SECRET_PROTECTION_AT_REST_RESERVED_LENGTH 8U

enum appliance_secret_protection_kind {
    APPLIANCE_SECRET_PROTECTION_KIND_NONE = 0,
    APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL = 1,
    APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN = 2,
    APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY = 3,
    APPLIANCE_SECRET_PROTECTION_KIND_OTHER_SECRET = 4,
};

enum appliance_secret_protection_state {
    APPLIANCE_SECRET_PROTECTION_STATE_EMPTY = 0,
    APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED = 1,
};

enum appliance_secret_protection_status {
    APPLIANCE_SECRET_PROTECTION_OK = 0,
    APPLIANCE_SECRET_PROTECTION_NULL = 1,
    APPLIANCE_SECRET_PROTECTION_BAD_VERSION = 2,
    APPLIANCE_SECRET_PROTECTION_BAD_STATE = 3,
    APPLIANCE_SECRET_PROTECTION_BAD_KIND = 4,
    APPLIANCE_SECRET_PROTECTION_BAD_LENGTH = 5,
    APPLIANCE_SECRET_PROTECTION_EMPTY_MATERIAL = 6,
    APPLIANCE_SECRET_PROTECTION_ALL_ZERO_MATERIAL = 7,
    APPLIANCE_SECRET_PROTECTION_EMPTY = 8,
    APPLIANCE_SECRET_PROTECTION_ALREADY_PROTECTED = 9,
    APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH = 10,
    APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED = 11,
    APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED = 12,
    APPLIANCE_SECRET_PROTECTION_VERIFY_FAILED = 13,
};

struct appliance_secret_material {
    uint32_t version;
    enum appliance_secret_protection_kind kind;
    const uint8_t *bytes;
    uint32_t length;
};

struct appliance_secret_protection {
    uint32_t version;
    enum appliance_secret_protection_state state;
    enum appliance_secret_protection_kind kind;
    uint32_t generation;
    bool at_rest_supported;
    bool material_retained;
    uint32_t retained_length;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    uint8_t retained_material[APPLIANCE_SECRET_PROTECTION_MAX_SECRET_LENGTH];
};

struct appliance_secret_protection_at_rest_record {
    uint32_t version;
    uint32_t kind;
    uint32_t generation;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    uint8_t reserved[APPLIANCE_SECRET_PROTECTION_AT_REST_RESERVED_LENGTH];
};

enum appliance_secret_protection_status
appliance_secret_protection_init(
    struct appliance_secret_protection *protection);
enum appliance_secret_protection_status
appliance_secret_protection_protect(
    struct appliance_secret_protection *protection,
    const struct appliance_secret_material *material);
enum appliance_secret_protection_status
appliance_secret_protection_rotate(
    struct appliance_secret_protection *protection,
    const struct appliance_secret_material *material);
enum appliance_secret_protection_status
appliance_secret_protection_reset(
    struct appliance_secret_protection *protection);
enum appliance_secret_protection_status
appliance_secret_protection_verify(
    const struct appliance_secret_protection *protection,
    const struct appliance_secret_material *material);
enum appliance_secret_protection_status
appliance_secret_protection_export_at_rest(
    const struct appliance_secret_protection *protection,
    struct appliance_secret_protection_at_rest_record *record);
/*
 * ITEM-401: the record fingerprint is the only credential material the
 * first-boot flow commits, so the verification owner recomputes it through
 * this one function instead of carrying a second derivation.  It is the same
 * domain-separated SHA-256 the protection record itself uses.
 */
enum appliance_secret_protection_status
appliance_secret_protection_fingerprint(
    enum appliance_secret_protection_kind kind,
    uint32_t generation,
    const uint8_t *bytes,
    uint32_t length,
    uint8_t digest[SHA256_DIGEST_SIZE]);
const char *appliance_secret_protection_status_string(
    enum appliance_secret_protection_status status);
const char *appliance_secret_protection_kind_string(
    enum appliance_secret_protection_kind kind);
const char *appliance_secret_protection_state_string(
    enum appliance_secret_protection_state state);

#endif
