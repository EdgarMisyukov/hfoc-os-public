#ifndef HFOC_APPLIANCE_SECRET_PERSISTENCE_H
#define HFOC_APPLIANCE_SECRET_PERSISTENCE_H

#include "appliance_persistent_storage_namespace.h"
#include "config_journal.h"
#include "https_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_SECRET_PERSISTENCE_VERSION 1U
#define APPLIANCE_SECRET_PERSISTENCE_RECORD_COUNT 4U
#define APPLIANCE_SECRET_PERSISTENCE_RECORD_MAX CONFIG_JOURNAL_DATA_MAX
#define APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH 64U

#define APPLIANCE_SECRET_PERSISTENCE_HTTPS_DEPENDENCY_READY \
    HTTPS_SERVICE_DEPENDENCY_CERTIFICATE_READY

enum appliance_secret_persistence_record_kind {
    APPLIANCE_SECRET_PERSISTENCE_KIND_NONE = 0,
    APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY = 1,
    APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL = 2,
    APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST = 3,
    APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED = 4,
};

enum appliance_secret_persistence_handling {
    APPLIANCE_SECRET_PERSISTENCE_HANDLING_NONE = 0,
    APPLIANCE_SECRET_PERSISTENCE_HANDLING_CERTIFICATE_RAW = 1,
    APPLIANCE_SECRET_PERSISTENCE_HANDLING_FINGERPRINT_ONLY = 2,
    APPLIANCE_SECRET_PERSISTENCE_HANDLING_DIGEST_ONLY = 3,
    APPLIANCE_SECRET_PERSISTENCE_HANDLING_WRAPPED_ONLY = 4,
    /*
     * ITEM-506: the administrator record now carries a PBKDF2-HMAC-SHA256
     * verifier with its salt and iteration count instead of only the
     * fingerprint of a single SHA-256.  The legacy fingerprint value is still
     * accepted by the catalogue so an old record is read as itself and
     * refused as a credential, not mistaken for corruption.
     */
    APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER = 5,
};

enum appliance_secret_persistence_status {
    APPLIANCE_SECRET_PERSISTENCE_OK = 0,
    APPLIANCE_SECRET_PERSISTENCE_NULL = 1,
    APPLIANCE_SECRET_PERSISTENCE_OUT_OF_RANGE = 2,
    APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD = 3,
    APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION = 4,
    APPLIANCE_SECRET_PERSISTENCE_UNKNOWN_KEY = 5,
    APPLIANCE_SECRET_PERSISTENCE_BAD_ID = 6,
    APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE = 7,
    APPLIANCE_SECRET_PERSISTENCE_BAD_STATE = 8,
};

struct appliance_secret_persistence_record {
    enum appliance_secret_persistence_record_kind kind;
    enum appliance_persistent_storage_namespace_id namespace_id;
    enum appliance_secret_persistence_handling handling;
    const char *name;
    const char *prefix;
    uint32_t version;
    uint32_t minimum_version;
    uint32_t maximum_version;
    const char *purpose;
};

struct appliance_secret_persistence_https_state {
    uint32_t version;
    bool certificate_published;
    bool private_key_wrapped_published;
};

size_t appliance_secret_persistence_count(void);
enum appliance_secret_persistence_status
appliance_secret_persistence_catalogue_at(
    size_t index,
    const struct appliance_secret_persistence_record **record_out);
enum appliance_secret_persistence_status
appliance_secret_persistence_describe(
    enum appliance_secret_persistence_record_kind kind,
    const struct appliance_secret_persistence_record **record_out);
enum appliance_secret_persistence_status
appliance_secret_persistence_classify_record_key(
    const char *key,
    size_t key_length,
    enum appliance_secret_persistence_record_kind *kind_out,
    enum appliance_persistent_storage_namespace_id *namespace_out,
    uint32_t *version_out);
enum appliance_secret_persistence_status
appliance_secret_persistence_validate_value(
    enum appliance_secret_persistence_record_kind kind,
    const uint8_t *value,
    size_t value_length);
enum appliance_secret_persistence_status
appliance_secret_persistence_https_readiness(
    const struct appliance_secret_persistence_https_state *state,
    uint32_t *dependency_mask_out,
    bool *ready_out);
const char *appliance_secret_persistence_record_kind_string(
    enum appliance_secret_persistence_record_kind kind);
const char *appliance_secret_persistence_handling_string(
    enum appliance_secret_persistence_handling handling);
const char *appliance_secret_persistence_status_string(
    enum appliance_secret_persistence_status status);

#endif
