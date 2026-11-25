#include "appliance_config_validation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char appliance_config_validation_prefix[] =
    APPLIANCE_CONFIG_VALIDATION_FORMAT_PREFIX;
static const char appliance_config_validation_key_with_equals[] =
    "system.hostname=";
static const char appliance_config_validation_language_prefix[] =
    APPLIANCE_CONFIG_VALIDATION_LANGUAGE_PREFIX;
static const char appliance_config_validation_panel_after_boot_prefix[] =
    APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_PREFIX;
static const char appliance_config_validation_panel_after_boot_open[] =
    APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_OPEN;
static const char appliance_config_validation_panel_after_boot_closed[] =
    APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_CLOSED;
static const char appliance_config_validation_default[] =
    APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME;

static void appliance_config_validation_copy_bytes(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static bool appliance_config_validation_bytes_equal(
    const void *left,
    const void *right,
    size_t length)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_config_validation_hostname_char_valid(
    unsigned char value)
{
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'-';
}

static bool appliance_config_validation_hostname_valid(
    const char *hostname,
    size_t hostname_length)
{
    size_t index;

    if (hostname == NULL ||
        hostname_length == 0U ||
        hostname_length > APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX ||
        hostname[0] == '-' ||
        hostname[hostname_length - 1U] == '-') {
        return false;
    }
    for (index = 0U; index < hostname_length; ++index) {
        if (!appliance_config_validation_hostname_char_valid(
                (unsigned char)hostname[index])) {
            return false;
        }
    }
    return true;
}

enum appliance_config_validation_status
appliance_config_validation_validate_hostname(
    const char *hostname,
    size_t hostname_length)
{
    if (hostname == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (!appliance_config_validation_hostname_valid(
            hostname, hostname_length)) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME;
    }
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

enum appliance_config_validation_status
appliance_config_validation_publish_hostname(
    struct appliance_config_validation_state *state,
    const char *hostname,
    size_t hostname_length,
    enum appliance_config_validation_source source)
{
    enum appliance_config_validation_status status;

    if (state == NULL || hostname == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (state->initialized) {
        if (state->source == APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
            return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
        }
    } else if (state->source != APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    if (source != APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    status = appliance_config_validation_validate_hostname(
        hostname, hostname_length);
    if (status != APPLIANCE_CONFIG_VALIDATION_OK) {
        return status;
    }

    appliance_config_validation_copy_bytes(
        state->hostname, hostname, hostname_length);
    state->hostname[hostname_length] = '\0';
    state->hostname_length = (uint8_t)hostname_length;
    state->source = source;
    state->initialized = true;
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

enum appliance_config_validation_status
appliance_config_validation_publish_default(
    struct appliance_config_validation_state *state,
    enum appliance_config_validation_source source)
{
    size_t default_length = sizeof(appliance_config_validation_default) - 1U;

    if (state == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (state->initialized) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    if (source != APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING &&
        source != APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    if (default_length > APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }

    appliance_config_validation_copy_bytes(
        state->hostname, appliance_config_validation_default,
        default_length);
    state->hostname[default_length] = '\0';
    state->hostname_length = (uint8_t)default_length;
    state->language = APPLIANCE_LANGUAGE_DEFAULT;
    /* ITEM-516: a default record never enabled the after-boot policy. */
    state->panel_after_boot_open = false;
    state->panel_after_boot_present = false;
    state->source = source;
    state->initialized = true;
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

enum appliance_config_validation_status
appliance_config_validation_publish_language(
    struct appliance_config_validation_state *state,
    enum appliance_language language,
    enum appliance_config_validation_source source)
{
    if (state == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (state->initialized) {
        if (state->source == APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
            return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
        }
    } else if (state->source != APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    if (source != APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    if (language != APPLIANCE_LANGUAGE_EN &&
        language != APPLIANCE_LANGUAGE_RU) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE;
    }

    state->language = language;
    state->source = source;
    state->initialized = true;
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

/*
 * ITEM-516: the optional third key of the record.  The value grammar is the
 * two words the record may hold; `closed` is accepted for completeness (a
 * reader must not refuse a record another writer produced) even though this
 * build never writes it.  Anything else is refused by name, never treated as
 * the default.
 */
const char *
appliance_config_validation_panel_after_boot_value(bool open)
{
    return open ? appliance_config_validation_panel_after_boot_open
                : appliance_config_validation_panel_after_boot_closed;
}

enum appliance_config_validation_status
appliance_config_validation_validate_panel_after_boot(
    const char *value,
    size_t value_length,
    bool *open_out)
{
    if (value == NULL || open_out == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (value_length ==
            sizeof(APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_OPEN) - 1U &&
        appliance_config_validation_bytes_equal(
            value, appliance_config_validation_panel_after_boot_open,
            value_length)) {
        *open_out = true;
        return APPLIANCE_CONFIG_VALIDATION_OK;
    }
    if (value_length ==
            sizeof(APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_CLOSED) - 1U &&
        appliance_config_validation_bytes_equal(
            value, appliance_config_validation_panel_after_boot_closed,
            value_length)) {
        *open_out = false;
        return APPLIANCE_CONFIG_VALIDATION_OK;
    }
    return APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT;
}

enum appliance_config_validation_status
appliance_config_validation_publish_panel_after_boot(
    struct appliance_config_validation_state *state,
    bool open,
    enum appliance_config_validation_source source)
{
    if (state == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (state->initialized) {
        if (state->source == APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
            return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
        }
    } else if (state->source != APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    if (source != APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }

    state->panel_after_boot_open = open;
    state->panel_after_boot_present = true;
    state->source = source;
    state->initialized = true;
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

/*
 * The record of the previous format is one `v1:system.hostname=` line; the
 * language adds an optional `v1:system.language=` line and ITEM-516 adds an
 * optional `v1:security.panel.after-boot=` line, each at most once and in any
 * order.  A line that names neither key is refused by name, a repeated key is
 * a malformed record, and the single-line record keeps reading exactly as it
 * did, with the default language and the default closed gate.
 */
enum appliance_config_validation_status
appliance_config_validation_validate_record(
    const void *record,
    size_t record_length,
    struct appliance_config_validation_state *state)
{
    const unsigned char *bytes = record;
    size_t prefix_length =
        sizeof(appliance_config_validation_prefix) - 1U;
    size_t key_length =
        sizeof(appliance_config_validation_key_with_equals) - 1U;
    size_t language_prefix_length =
        sizeof(appliance_config_validation_language_prefix) - 1U;
    size_t panel_prefix_length =
        sizeof(appliance_config_validation_panel_after_boot_prefix) - 1U;
    size_t hostname_start;
    size_t hostname_length;
    size_t line_start;
    size_t line_end;
    size_t value_start;
    size_t value_length;
    enum appliance_language language;
    bool language_seen = false;
    bool panel_seen = false;
    bool panel_open = false;
    enum appliance_config_validation_status status;

    if (record == NULL || state == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (state->initialized) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    if (record_length == 0U ||
        record_length > APPLIANCE_CONFIG_VALIDATION_RECORD_MAX ||
        bytes[record_length - 1U] != (unsigned char)'\n') {
        return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
    }
    if (record_length < 3U ||
        bytes[0] != (unsigned char)'v' ||
        bytes[2] != (unsigned char)':') {
        return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
    }
    if (bytes[1] < (unsigned char)'0' ||
        bytes[1] > (unsigned char)'9') {
        return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
    }
    if (bytes[1] != (unsigned char)'1') {
        return APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION;
    }

    line_end = 0U;
    while (line_end < record_length &&
           bytes[line_end] != (unsigned char)'\n') {
        ++line_end;
    }
    if (line_end < 3U + key_length ||
        !appliance_config_validation_bytes_equal(
            bytes + 3U, appliance_config_validation_key_with_equals,
            key_length)) {
        return APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY;
    }
    hostname_start = 3U + key_length;
    hostname_length = line_end - hostname_start;
    if (!appliance_config_validation_hostname_valid(
            (const char *)bytes + hostname_start, hostname_length)) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME;
    }
    if (line_end < prefix_length ||
        !appliance_config_validation_bytes_equal(
            bytes, appliance_config_validation_prefix,
            prefix_length)) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
    }

    status = appliance_config_validation_publish_hostname(
        state, (const char *)bytes + hostname_start, hostname_length,
        APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED);
    if (status != APPLIANCE_CONFIG_VALIDATION_OK) {
        return status;
    }
    if (line_end + 1U == record_length) {
        state->language = APPLIANCE_LANGUAGE_DEFAULT;
        return APPLIANCE_CONFIG_VALIDATION_OK;
    }

    line_start = line_end + 1U;
    while (line_start < record_length) {
        line_end = line_start;
        while (line_end < record_length &&
               bytes[line_end] != (unsigned char)'\n') {
            ++line_end;
        }
        if (line_end >= record_length) {
            return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
        }
        if (line_end < line_start + 3U ||
            bytes[line_start] != (unsigned char)'v' ||
            bytes[line_start + 2U] != (unsigned char)':') {
            return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
        }
        if (bytes[line_start + 1U] < (unsigned char)'0' ||
            bytes[line_start + 1U] > (unsigned char)'9') {
            return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
        }
        if (bytes[line_start + 1U] != (unsigned char)'1') {
            return APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION;
        }
        if (line_end >= line_start + language_prefix_length &&
            appliance_config_validation_bytes_equal(
                bytes + line_start,
                appliance_config_validation_language_prefix,
                language_prefix_length)) {
            if (language_seen) {
                return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
            }
            value_start = line_start + language_prefix_length;
            value_length = line_end - value_start;
            if (appliance_language_parse(
                    (const char *)bytes + value_start, value_length,
                    &language) != APPLIANCE_LANGUAGE_OK) {
                return APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE;
            }
            status = appliance_config_validation_publish_language(
                state, language,
                APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED);
            if (status != APPLIANCE_CONFIG_VALIDATION_OK) {
                return status;
            }
            language_seen = true;
        } else if (line_end >= line_start + panel_prefix_length &&
                   appliance_config_validation_bytes_equal(
                       bytes + line_start,
                       appliance_config_validation_panel_after_boot_prefix,
                       panel_prefix_length)) {
            if (panel_seen) {
                return APPLIANCE_CONFIG_VALIDATION_BAD_RECORD;
            }
            value_start = line_start + panel_prefix_length;
            value_length = line_end - value_start;
            status = appliance_config_validation_validate_panel_after_boot(
                (const char *)bytes + value_start, value_length,
                &panel_open);
            if (status != APPLIANCE_CONFIG_VALIDATION_OK) {
                return status;
            }
            state->panel_after_boot_open = panel_open;
            state->panel_after_boot_present = true;
            panel_seen = true;
        } else {
            return APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY;
        }
        line_start = line_end + 1U;
    }
    if (!language_seen) {
        state->language = APPLIANCE_LANGUAGE_DEFAULT;
    }
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

enum appliance_config_validation_source
appliance_config_validation_source(
    const struct appliance_config_validation_state *state)
{
    if (state == NULL || !state->initialized) {
        return APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE;
    }
    return state->source;
}

enum appliance_config_validation_status
appliance_config_validation_hostname(
    const struct appliance_config_validation_state *state,
    char *hostname,
    size_t capacity,
    size_t *hostname_length)
{
    if (state == NULL || hostname == NULL ||
        hostname_length == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (!state->initialized ||
        state->source == APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE ||
        state->hostname_length == 0U ||
        state->hostname_length >
            APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    if (capacity < (size_t)state->hostname_length + 1U) {
        return APPLIANCE_CONFIG_VALIDATION_BUFFER_TOO_SMALL;
    }
    appliance_config_validation_copy_bytes(
        hostname, state->hostname, state->hostname_length);
    hostname[state->hostname_length] = '\0';
    *hostname_length = state->hostname_length;
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

enum appliance_config_validation_status
appliance_config_validation_language(
    const struct appliance_config_validation_state *state,
    enum appliance_language *language)
{
    if (state == NULL || language == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (!state->initialized ||
        state->source == APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    *language = state->language;
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

enum appliance_config_validation_status
appliance_config_validation_panel_after_boot(
    const struct appliance_config_validation_state *state,
    bool *open_out)
{
    if (state == NULL || open_out == NULL) {
        return APPLIANCE_CONFIG_VALIDATION_NULL;
    }
    if (!state->initialized ||
        state->source == APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
        return APPLIANCE_CONFIG_VALIDATION_BAD_STATE;
    }
    /*
     * A record that predates the key means the default policy, not an unknown
     * one: the gate stays closed exactly as it did before the key existed.
     */
    *open_out = state->panel_after_boot_present &&
                state->panel_after_boot_open;
    return APPLIANCE_CONFIG_VALIDATION_OK;
}

const char *appliance_config_validation_status_string(
    enum appliance_config_validation_status status)
{
    switch (status) {
    case APPLIANCE_CONFIG_VALIDATION_OK:
        return "APPLIANCE_CONFIG_VALIDATION_OK";
    case APPLIANCE_CONFIG_VALIDATION_NULL:
        return "APPLIANCE_CONFIG_VALIDATION_NULL";
    case APPLIANCE_CONFIG_VALIDATION_BAD_STATE:
        return "APPLIANCE_CONFIG_VALIDATION_BAD_STATE";
    case APPLIANCE_CONFIG_VALIDATION_BAD_RECORD:
        return "APPLIANCE_CONFIG_VALIDATION_BAD_RECORD";
    case APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION:
        return "APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION";
    case APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY:
        return "APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY";
    case APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME:
        return "APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME";
    case APPLIANCE_CONFIG_VALIDATION_BUFFER_TOO_SMALL:
        return "APPLIANCE_CONFIG_VALIDATION_BUFFER_TOO_SMALL";
    case APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE:
        return "APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE";
    case APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT:
        return "APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT";
    default:
        return NULL;
    }
}
