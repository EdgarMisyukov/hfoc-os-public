#ifndef HFOC_APPLIANCE_PERSISTENT_SECRET_CONFIG_H
#define HFOC_APPLIANCE_PERSISTENT_SECRET_CONFIG_H

#include "appliance_admin_credential.h"
#include "block_device.h"
#include "config_journal.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PERSISTENT_SECRET_CONFIG_FORMAT_VERSION 1U
#define APPLIANCE_PERSISTENT_SECRET_CONFIG_KEY \
    "secret.admin.credential"
#define APPLIANCE_PERSISTENT_SECRET_CONFIG_PREFIX \
    "v1:secret.admin.credential="
#define APPLIANCE_PERSISTENT_SECRET_CONFIG_HEX_LENGTH 64U
#define APPLIANCE_PERSISTENT_SECRET_CONFIG_RECORD_MAX \
    CONFIG_JOURNAL_DATA_MAX
#define APPLIANCE_PERSISTENT_SECRET_CONFIG_VALUE_MAX \
    APPLIANCE_ADMIN_CREDENTIAL_RECORD_VALUE_MAX

enum appliance_persistent_secret_config_status {
    APPLIANCE_PERSISTENT_SECRET_CONFIG_OK = 0,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL = 1,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE = 2,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_DEVICE = 3,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_IO_ERROR = 4,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_CORRUPT = 5,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_FULL = 6,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_RECORD = 7,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE = 8,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_UNSUPPORTED = 9,
    /*
     * ITEM-506: the store holds no administrator record at all, and the store
     * holds the legacy single-SHA-256 fingerprint record, which is not a
     * credential.  Both are named, so a caller can tell "unprovisioned" from
     * "old record needs re-provisioning" instead of guessing from a boolean.
     */
    APPLIANCE_PERSISTENT_SECRET_CONFIG_ABSENT = 10,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_LEGACY_RECORD = 11
};

enum appliance_persistent_secret_config_source {
    APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_NONE = 0,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_COMMITTED = 1,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_MISSING = 2,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_SOURCE_DEFAULT_INVALID = 3,
};

/*
 * What the head of the administrator journal is.  The legacy value is the
 * 64-hex fingerprint the pre-ITEM-506 flow wrote; it is kept readable as
 * itself and is never presented as a credential.
 */
enum appliance_persistent_secret_config_admin_kind {
    APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_NONE = 0,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_LEGACY_FINGERPRINT = 1,
    APPLIANCE_PERSISTENT_SECRET_CONFIG_ADMIN_PBKDF2_CREDENTIAL = 2,
};

struct appliance_persistent_secret_config {
    struct config_journal journal;
    enum appliance_persistent_secret_config_source source;
    bool initialized;
    bool admin_present;
    uint8_t admin_fingerprint[SHA256_DIGEST_SIZE];
    char admin_hex[APPLIANCE_PERSISTENT_SECRET_CONFIG_HEX_LENGTH + 1U];
    enum appliance_persistent_secret_config_admin_kind admin_kind;
    bool admin_credential_present;
    struct appliance_admin_credential admin_credential;
};

enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_load(
    struct appliance_persistent_secret_config *config,
    struct block_device *device);
/*
 * Legacy fingerprint writer.  ITEM-506 keeps it for the first-boot factory
 * placeholder and for tests of the old record, but a record written this way
 * is not a credential: `appliance_persistent_secret_config_admin_credential()`
 * refuses it with LEGACY_RECORD.
 */
enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_commit_admin(
    struct appliance_persistent_secret_config *config,
    const uint8_t fingerprint[SHA256_DIGEST_SIZE]);
/*
 * ITEM-506 credential writer.  The credential is validated by the credential
 * module (including its login, salt, iteration count and verifier), encoded
 * into the canonical record value and appended to the same journal key, so
 * the newest head is always the current credential and the old fingerprint
 * record stays readable behind it.
 */
enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_commit_admin_credential(
    struct appliance_persistent_secret_config *config,
    const struct appliance_admin_credential *credential);
bool appliance_persistent_secret_config_admin(
    const struct appliance_persistent_secret_config *config,
    uint8_t fingerprint_out[SHA256_DIGEST_SIZE]);
/*
 * Read the structured credential.  ABSENT means no administrator record,
 * LEGACY_RECORD means only the old fingerprint record exists; both leave
 * `credential_out` zeroed.
 */
enum appliance_persistent_secret_config_status
appliance_persistent_secret_config_admin_credential(
    const struct appliance_persistent_secret_config *config,
    struct appliance_admin_credential *credential_out);
enum appliance_persistent_secret_config_admin_kind
appliance_persistent_secret_config_admin_kind(
    const struct appliance_persistent_secret_config *config);
enum appliance_persistent_secret_config_source
appliance_persistent_secret_config_source(
    const struct appliance_persistent_secret_config *config);
const char *appliance_persistent_secret_config_status_string(
    enum appliance_persistent_secret_config_status status);
const char *appliance_persistent_secret_config_source_string(
    enum appliance_persistent_secret_config_source source);

#endif
