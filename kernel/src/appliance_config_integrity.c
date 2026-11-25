#include "appliance_config_integrity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const struct appliance_config_integrity_record
    appliance_config_integrity_records[] = {
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_HOSTNAME,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "system.hostname",
        "v1:system.hostname=",
        1U,
        1U,
        1U,
        "validated appliance hostname",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_LANGUAGE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "system.language",
        "v1:system.language=",
        1U,
        1U,
        1U,
        "validated interface language",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SECURITY_PANEL_AFTER_BOOT,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "security.panel.after-boot",
        "v1:security.panel.after-boot=",
        1U,
        1U,
        1U,
        "validated management gate after-boot policy",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_INTERFACE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.interface",
        "v1:network.interface=",
        1U,
        1U,
        1U,
        "validated network interface identity",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_MODE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.mode",
        "v1:network.mode=",
        1U,
        1U,
        1U,
        "validated DHCP or STATIC address mode",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_ADDRESS,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.ipv4_address",
        "v1:network.ipv4_address=",
        1U,
        1U,
        1U,
        "validated static IPv4 address",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_PREFIX,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.ipv4_prefix",
        "v1:network.ipv4_prefix=",
        1U,
        1U,
        1U,
        "validated static IPv4 prefix length",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_GATEWAY,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.ipv4_gateway",
        "v1:network.ipv4_gateway=",
        1U,
        1U,
        1U,
        "validated static IPv4 gateway",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_DNS,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.ipv4_dns",
        "v1:network.ipv4_dns=",
        1U,
        1U,
        1U,
        "validated static IPv4 DNS server",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_TABLE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        "service.table",
        "v1:services=",
        1U,
        1U,
        1U,
        "validated persistent service table count",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        "service.entry",
        "v1:service.",
        1U,
        1U,
        1U,
        "validated persistent service enablement entry",
    },
};

static void appliance_config_integrity_publish_none(
    enum appliance_config_integrity_record_kind *kind_out,
    enum appliance_persistent_storage_namespace_id *namespace_out,
    uint32_t *version_out)
{
    if (kind_out != NULL) {
        *kind_out = APPLIANCE_CONFIG_INTEGRITY_KIND_NONE;
    }
    if (namespace_out != NULL) {
        *namespace_out = APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    }
    if (version_out != NULL) {
        *version_out = 0U;
    }
}

static bool appliance_config_integrity_bytes_equal(
    const char *left,
    const char *right,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if ((unsigned char)left[index] !=
            (unsigned char)right[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_config_integrity_has_prefix(
    const char *key,
    size_t key_length,
    const char *prefix,
    size_t prefix_length)
{
    return key_length > prefix_length &&
           appliance_config_integrity_bytes_equal(
               key, prefix, prefix_length);
}

static size_t appliance_config_integrity_string_length(
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool appliance_config_integrity_digit_valid(
    unsigned char value)
{
    return value >= (unsigned char)'0' &&
           value <= (unsigned char)'9';
}

static bool appliance_config_integrity_decimal(
    const char *value,
    size_t value_length,
    uint32_t *parsed_out)
{
    uint32_t parsed = 0U;
    size_t index;

    if (value == NULL || parsed_out == NULL ||
        value_length == 0U ||
        value_length > 3U ||
        (value_length > 1U && value[0] == '0')) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        const unsigned char byte =
            (unsigned char)value[index];

        if (!appliance_config_integrity_digit_valid(byte)) {
            return false;
        }
        if (parsed >
            (UINT32_MAX - (uint32_t)(byte - (unsigned char)'0')) / 10U) {
            return false;
        }
        parsed = parsed * 10U +
                 (uint32_t)(byte - (unsigned char)'0');
    }
    *parsed_out = parsed;
    return true;
}

static enum appliance_config_integrity_status
appliance_config_integrity_classify_service_entry(
    const char *key,
    size_t key_length,
    enum appliance_config_integrity_record_kind *kind_out,
    enum appliance_persistent_storage_namespace_id *namespace_out,
    uint32_t *version_out)
{
    static const char service_prefix[] = "v1:service.";
    const size_t prefix_length =
        sizeof(service_prefix) - 1U;
    size_t position;
    size_t id_start;
    size_t id_length;
    uint32_t service_id = 0U;

    if (key_length <= prefix_length ||
        !appliance_config_integrity_bytes_equal(
            key, service_prefix, prefix_length)) {
        return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
    }

    position = prefix_length;
    id_start = position;
    while (position < key_length &&
           key[position] != (unsigned char)'=') {
        ++position;
    }
    if (position == id_start || position >= key_length) {
        return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
    }
    id_length = position - id_start;
    if (!appliance_config_integrity_decimal(
            key + id_start, id_length, &service_id) ||
        service_id < APPLIANCE_CONFIG_INTEGRITY_SERVICE_ID_MIN ||
        service_id > APPLIANCE_CONFIG_INTEGRITY_SERVICE_ID_MAX) {
        return APPLIANCE_CONFIG_INTEGRITY_BAD_ID;
    }
    if (position + 1U >= key_length) {
        return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
    }

    *kind_out = APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY;
    *namespace_out = APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA;
    *version_out = 1U;
    return APPLIANCE_CONFIG_INTEGRITY_OK;
}

static bool appliance_config_integrity_starts_with(
    const char *key,
    size_t key_length,
    const char *text,
    size_t text_length)
{
    return key_length >= text_length &&
           appliance_config_integrity_bytes_equal(
               key, text, text_length);
}

size_t appliance_config_integrity_count(void)
{
    return sizeof(appliance_config_integrity_records) /
        sizeof(appliance_config_integrity_records[0]);
}

enum appliance_config_integrity_status
appliance_config_integrity_catalogue_at(
    size_t index,
    const struct appliance_config_integrity_record **record_out)
{
    if (record_out == NULL) {
        return APPLIANCE_CONFIG_INTEGRITY_NULL;
    }
    if (index >= appliance_config_integrity_count()) {
        *record_out = NULL;
        return APPLIANCE_CONFIG_INTEGRITY_OUT_OF_RANGE;
    }
    *record_out = &appliance_config_integrity_records[index];
    return APPLIANCE_CONFIG_INTEGRITY_OK;
}

enum appliance_config_integrity_status
appliance_config_integrity_describe(
    enum appliance_config_integrity_record_kind kind,
    const struct appliance_config_integrity_record **record_out)
{
    size_t index;

    if (record_out == NULL) {
        return APPLIANCE_CONFIG_INTEGRITY_NULL;
    }
    for (index = 0U;
         index < appliance_config_integrity_count();
         ++index) {
        if (appliance_config_integrity_records[index].kind == kind) {
            *record_out = &appliance_config_integrity_records[index];
            return APPLIANCE_CONFIG_INTEGRITY_OK;
        }
    }
    *record_out = NULL;
    return APPLIANCE_CONFIG_INTEGRITY_BAD_ID;
}

enum appliance_config_integrity_status
appliance_config_integrity_classify_record_key(
    const char *key,
    size_t key_length,
    enum appliance_config_integrity_record_kind *kind_out,
    enum appliance_persistent_storage_namespace_id *namespace_out,
    uint32_t *version_out)
{
    size_t index;
    size_t position;

    if (key == NULL || kind_out == NULL ||
        namespace_out == NULL || version_out == NULL) {
        return APPLIANCE_CONFIG_INTEGRITY_NULL;
    }
    appliance_config_integrity_publish_none(
        kind_out, namespace_out, version_out);
    if (key_length == 0U ||
        key_length > APPLIANCE_CONFIG_INTEGRITY_RECORD_MAX) {
        return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
    }
    if (key_length > 0U &&
        key[key_length - 1U] == (unsigned char)'\n') {
        --key_length;
    }
    if (key_length < 3U ||
        key[0] != (unsigned char)'v' ||
        key[2] != (unsigned char)':') {
        return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
    }
    if (!appliance_config_integrity_digit_valid(
            (unsigned char)key[1])) {
        return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
    }
    if (key[1] != (unsigned char)'1') {
        return APPLIANCE_CONFIG_INTEGRITY_BAD_VERSION;
    }
    for (position = 0U; position < key_length; ++position) {
        if (key[position] == (unsigned char)'\n') {
            return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
        }
    }

    for (index = 0U;
         index < appliance_config_integrity_count();
         ++index) {
        const struct appliance_config_integrity_record *record =
            &appliance_config_integrity_records[index];
        const size_t prefix_length =
            appliance_config_integrity_string_length(record->prefix);

        if (record->kind ==
            APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY) {
            continue;
        }
        if (appliance_config_integrity_starts_with(
                key, key_length, record->prefix, prefix_length)) {
            if (key_length == prefix_length) {
                return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
            }
            *kind_out = record->kind;
            *namespace_out = record->namespace_id;
            *version_out = record->version;
            return APPLIANCE_CONFIG_INTEGRITY_OK;
        }
        if (key_length + 1U == prefix_length &&
            appliance_config_integrity_starts_with(
                key, key_length, record->prefix, key_length) &&
            record->prefix[key_length] == (unsigned char)'=') {
            return APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD;
        }
        if (appliance_config_integrity_has_prefix(
                key, key_length, record->prefix, prefix_length)) {
            *kind_out = record->kind;
            *namespace_out = record->namespace_id;
            *version_out = record->version;
            return APPLIANCE_CONFIG_INTEGRITY_OK;
        }
    }

    if (appliance_config_integrity_starts_with(
            key, key_length, "v1:service.", 11U)) {
        return appliance_config_integrity_classify_service_entry(
            key, key_length, kind_out, namespace_out, version_out);
    }

    appliance_config_integrity_publish_none(
        kind_out, namespace_out, version_out);
    return APPLIANCE_CONFIG_INTEGRITY_UNKNOWN_KEY;
}

enum appliance_config_integrity_recovery
appliance_config_integrity_recovery_plan(
    enum config_journal_status journal_status,
    bool record_valid,
    bool last_known_good_available)
{
    if (journal_status == CONFIG_JOURNAL_OK) {
        if (record_valid) {
            return APPLIANCE_CONFIG_INTEGRITY_RECOVERY_COMMIT;
        }
        if (last_known_good_available) {
            return APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD;
        }
        return APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_INVALID;
    }
    if (journal_status == CONFIG_JOURNAL_EMPTY) {
        return APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_MISSING;
    }
    return APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR;
}

const char *appliance_config_integrity_record_kind_string(
    enum appliance_config_integrity_record_kind kind)
{
    switch (kind) {
    case APPLIANCE_CONFIG_INTEGRITY_KIND_NONE:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_NONE";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_HOSTNAME:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_HOSTNAME";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_LANGUAGE:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_LANGUAGE";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_SECURITY_PANEL_AFTER_BOOT:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_SECURITY_PANEL_AFTER_BOOT";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_INTERFACE:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_INTERFACE";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_MODE:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_MODE";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_ADDRESS:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_ADDRESS";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_PREFIX:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_PREFIX";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_GATEWAY:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_GATEWAY";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_DNS:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_DNS";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_TABLE:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_TABLE";
    case APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY:
        return "APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY";
    default:
        return NULL;
    }
}

const char *appliance_config_integrity_recovery_string(
    enum appliance_config_integrity_recovery recovery)
{
    switch (recovery) {
    case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE:
        return "APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE";
    case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_COMMIT:
        return "APPLIANCE_CONFIG_INTEGRITY_RECOVERY_COMMIT";
    case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD:
        return "APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD";
    case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_MISSING:
        return "APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_MISSING";
    case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_INVALID:
        return "APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_INVALID";
    case APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR:
        return "APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR";
    default:
        return NULL;
    }
}

const char *appliance_config_integrity_status_string(
    enum appliance_config_integrity_status status)
{
    switch (status) {
    case APPLIANCE_CONFIG_INTEGRITY_OK:
        return "APPLIANCE_CONFIG_INTEGRITY_OK";
    case APPLIANCE_CONFIG_INTEGRITY_NULL:
        return "APPLIANCE_CONFIG_INTEGRITY_NULL";
    case APPLIANCE_CONFIG_INTEGRITY_OUT_OF_RANGE:
        return "APPLIANCE_CONFIG_INTEGRITY_OUT_OF_RANGE";
    case APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD:
        return "APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD";
    case APPLIANCE_CONFIG_INTEGRITY_BAD_VERSION:
        return "APPLIANCE_CONFIG_INTEGRITY_BAD_VERSION";
    case APPLIANCE_CONFIG_INTEGRITY_UNKNOWN_KEY:
        return "APPLIANCE_CONFIG_INTEGRITY_UNKNOWN_KEY";
    case APPLIANCE_CONFIG_INTEGRITY_BAD_ID:
        return "APPLIANCE_CONFIG_INTEGRITY_BAD_ID";
    default:
        return NULL;
    }
}
