#include "appliance_console.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool appliance_console_state_is_valid(
    enum appliance_console_state state)
{
    return state == APPLIANCE_CONSOLE_STATE_UNAVAILABLE ||
           state == APPLIANCE_CONSOLE_STATE_OK ||
           state == APPLIANCE_CONSOLE_STATE_DEGRADED ||
           state == APPLIANCE_CONSOLE_STATE_FAILED;
}

static bool appliance_console_source_is_valid(
    enum appliance_config_validation_source source)
{
    return source == APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED ||
           source == APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING ||
           source == APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID;
}

static bool appliance_console_copy_text(
    char *destination,
    size_t capacity,
    const char *source)
{
    size_t index;

    if (destination == NULL || source == NULL || capacity == 0U) {
        return false;
    }
    for (index = 0U;
         index + 1U < capacity && source[index] != '\0';
         ++index) {
        destination[index] = source[index];
    }
    if (source[index] != '\0') {
        return false;
    }
    destination[index] = '\0';
    return index != 0U;
}

static bool appliance_console_copy_hostname(
    char *destination,
    const char *source,
    size_t length)
{
    size_t index;

    if (destination == NULL || source == NULL ||
        length == 0U ||
        length > APPLIANCE_CONSOLE_HOSTNAME_MAX) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
    destination[length] = '\0';
    return true;
}

static enum appliance_console_status
appliance_console_validate_snapshot(
    const struct appliance_console_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return APPLIANCE_CONSOLE_NULL;
    }
    if (!snapshot->published) {
        return APPLIANCE_CONSOLE_BAD_STATE;
    }
    if (snapshot->version != APPLIANCE_CONSOLE_VERSION) {
        return APPLIANCE_CONSOLE_BAD_VERSION;
    }
    if (!snapshot->identity_published ||
        snapshot->product[0] == '\0' ||
        snapshot->architecture[0] == '\0' ||
        snapshot->platform[0] == '\0' ||
        snapshot->hostname[0] == '\0') {
        return APPLIANCE_CONSOLE_BAD_IDENTITY;
    }
    if (!appliance_console_state_is_valid(snapshot->config_state)) {
        return APPLIANCE_CONSOLE_BAD_CONFIG;
    }
    if (snapshot->config_state !=
            APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        if (!appliance_console_source_is_valid(snapshot->config_source) ||
            appliance_config_validation_validate_hostname(
                snapshot->config_hostname,
                strlen(snapshot->config_hostname)) !=
                APPLIANCE_CONFIG_VALIDATION_OK) {
            return APPLIANCE_CONSOLE_BAD_CONFIG;
        }
    } else if (snapshot->config_source !=
               APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE) {
        return APPLIANCE_CONSOLE_BAD_CONFIG;
    }
    if (!appliance_console_state_is_valid(snapshot->health_state)) {
        return APPLIANCE_CONSOLE_BAD_HEALTH;
    }
    if (!appliance_console_state_is_valid(snapshot->network_state)) {
        return APPLIANCE_CONSOLE_BAD_NETWORK;
    }
    if (snapshot->network_state !=
            APPLIANCE_CONSOLE_STATE_UNAVAILABLE &&
        (snapshot->network_devices == 0U ||
         snapshot->network_devices > APPLIANCE_CONSOLE_DOMAIN_MAX)) {
        return APPLIANCE_CONSOLE_BAD_NETWORK;
    }
    if (!appliance_console_state_is_valid(snapshot->services_state)) {
        return APPLIANCE_CONSOLE_BAD_SERVICE;
    }
    if (snapshot->services_state !=
            APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        if (snapshot->service_count == 0U ||
            snapshot->service_count > APPLIANCE_CONSOLE_DOMAIN_MAX ||
            snapshot->service_ready > snapshot->service_count ||
            snapshot->service_failed > snapshot->service_count ||
            snapshot->service_ready + snapshot->service_failed >
                snapshot->service_count) {
            return APPLIANCE_CONSOLE_BAD_SERVICE;
        }
    }
    return APPLIANCE_CONSOLE_OK;
}

static enum appliance_console_status
appliance_console_map_validation(
    enum appliance_config_validation_status status)
{
    switch (status) {
    case APPLIANCE_CONFIG_VALIDATION_OK:
        return APPLIANCE_CONSOLE_OK;
    case APPLIANCE_CONFIG_VALIDATION_NULL:
        return APPLIANCE_CONSOLE_NULL;
    default:
        return APPLIANCE_CONSOLE_BAD_CONFIG;
    }
}

enum appliance_console_status appliance_console_snapshot_init(
    struct appliance_console_snapshot *snapshot,
    const struct appliance_identity *identity,
    enum appliance_console_state health_state,
    uint64_t uptime_ticks)
{
    enum appliance_identity_status identity_status;

    if (snapshot == NULL || identity == NULL) {
        return APPLIANCE_CONSOLE_NULL;
    }
    identity_status = appliance_identity_validate(identity);
    if (identity_status != APPLIANCE_IDENTITY_OK) {
        return APPLIANCE_CONSOLE_BAD_IDENTITY;
    }
    if (!appliance_console_state_is_valid(health_state)) {
        return APPLIANCE_CONSOLE_BAD_HEALTH;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (!appliance_console_copy_text(
            snapshot->product, sizeof(snapshot->product),
            identity->product) ||
        !appliance_console_copy_text(
            snapshot->architecture, sizeof(snapshot->architecture),
            identity->architecture) ||
        !appliance_console_copy_text(
            snapshot->platform, sizeof(snapshot->platform),
            identity->platform) ||
        !appliance_console_copy_text(
            snapshot->hostname, sizeof(snapshot->hostname),
            identity->hostname)) {
        return APPLIANCE_CONSOLE_BAD_IDENTITY;
    }

    snapshot->version = APPLIANCE_CONSOLE_VERSION;
    snapshot->published = true;
    snapshot->identity_published = true;
    snapshot->config_state = APPLIANCE_CONSOLE_STATE_UNAVAILABLE;
    snapshot->config_source =
        APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE;
    snapshot->health_state = health_state;
    snapshot->uptime_ticks = uptime_ticks;
    snapshot->network_state = APPLIANCE_CONSOLE_STATE_UNAVAILABLE;
    snapshot->services_state = APPLIANCE_CONSOLE_STATE_UNAVAILABLE;
    return APPLIANCE_CONSOLE_OK;
}

enum appliance_console_status appliance_console_publish_config(
    struct appliance_console_snapshot *snapshot,
    enum appliance_config_validation_source source,
    const char *hostname,
    size_t hostname_length)
{
    enum appliance_config_validation_status validation_status;
    enum appliance_console_status status;

    if (snapshot == NULL || hostname == NULL) {
        return APPLIANCE_CONSOLE_NULL;
    }
    status = appliance_console_validate_snapshot(snapshot);
    if (status != APPLIANCE_CONSOLE_OK) {
        return status;
    }
    if (!appliance_console_source_is_valid(source)) {
        return APPLIANCE_CONSOLE_BAD_CONFIG;
    }
    validation_status =
        appliance_config_validation_validate_hostname(
            hostname, hostname_length);
    status = appliance_console_map_validation(validation_status);
    if (status != APPLIANCE_CONSOLE_OK) {
        return status;
    }
    if (!appliance_console_copy_hostname(
            snapshot->config_hostname, hostname, hostname_length)) {
        return APPLIANCE_CONSOLE_BAD_CONFIG;
    }
    snapshot->config_source = source;
    snapshot->config_state = APPLIANCE_CONSOLE_STATE_OK;
    return APPLIANCE_CONSOLE_OK;
}

enum appliance_console_status appliance_console_publish_network(
    struct appliance_console_snapshot *snapshot,
    enum appliance_console_state state,
    uint64_t generation,
    uint32_t device_count)
{
    enum appliance_console_status status;

    if (snapshot == NULL) {
        return APPLIANCE_CONSOLE_NULL;
    }
    status = appliance_console_validate_snapshot(snapshot);
    if (status != APPLIANCE_CONSOLE_OK) {
        return status;
    }
    if (!appliance_console_state_is_valid(state) ||
        state == APPLIANCE_CONSOLE_STATE_UNAVAILABLE ||
        device_count == 0U ||
        device_count > APPLIANCE_CONSOLE_DOMAIN_MAX) {
        return APPLIANCE_CONSOLE_BAD_NETWORK;
    }
    snapshot->network_state = state;
    snapshot->network_generation = generation;
    snapshot->network_devices = device_count;
    return APPLIANCE_CONSOLE_OK;
}

enum appliance_console_status appliance_console_publish_services(
    struct appliance_console_snapshot *snapshot,
    enum appliance_console_state state,
    uint32_t service_count,
    uint32_t ready_count,
    uint32_t failed_count)
{
    enum appliance_console_status status;

    if (snapshot == NULL) {
        return APPLIANCE_CONSOLE_NULL;
    }
    status = appliance_console_validate_snapshot(snapshot);
    if (status != APPLIANCE_CONSOLE_OK) {
        return status;
    }
    if (!appliance_console_state_is_valid(state) ||
        state == APPLIANCE_CONSOLE_STATE_UNAVAILABLE ||
        service_count == 0U ||
        service_count > APPLIANCE_CONSOLE_DOMAIN_MAX ||
        ready_count > service_count ||
        failed_count > service_count ||
        ready_count + failed_count > service_count) {
        return APPLIANCE_CONSOLE_BAD_SERVICE;
    }
    snapshot->services_state = state;
    snapshot->service_count = service_count;
    snapshot->service_ready = ready_count;
    snapshot->service_failed = failed_count;
    return APPLIANCE_CONSOLE_OK;
}

static const char *appliance_console_source_string(
    enum appliance_config_validation_source source)
{
    switch (source) {
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE:
        return "NONE";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED:
        return "COMMITTED";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING:
        return "DEFAULT_MISSING";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID:
        return "DEFAULT_INVALID";
    default:
        return NULL;
    }
}

static bool appliance_console_append_bytes(
    char *output,
    size_t capacity,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > capacity || length > capacity - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_console_append_cstr(
    char *output,
    size_t capacity,
    size_t *offset,
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return appliance_console_append_bytes(
        output, capacity, offset, text, length);
}

static bool appliance_console_append_u64(
    char *output,
    size_t capacity,
    size_t *offset,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        return appliance_console_append_bytes(
            output, capacity, offset, "0", 1U);
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        --count;
        if (!appliance_console_append_bytes(
                output, capacity, offset, &digits[count], 1U)) {
            return false;
        }
    }
    return true;
}

static bool appliance_console_append_newline(
    char *output,
    size_t capacity,
    size_t *offset)
{
    return appliance_console_append_bytes(
        output, capacity, offset, "\n", 1U);
}

enum appliance_console_status appliance_console_render(
    const struct appliance_console_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_CONSOLE_OUTPUT_MAX + 1U];
    enum appliance_console_status status;
    size_t offset = 0U;
    size_t index;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_CONSOLE_NULL;
    }
    status = appliance_console_validate_snapshot(snapshot);
    if (status != APPLIANCE_CONSOLE_OK) {
        return status;
    }

    if (!appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, "appliance v=") ||
        !appliance_console_append_u64(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, snapshot->version) ||
        !appliance_console_append_newline(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX, &offset) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, "identity product=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, snapshot->product) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, " arch=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, snapshot->architecture) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, " platform=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, snapshot->platform) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, " hostname=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, snapshot->hostname) ||
        !appliance_console_append_newline(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX, &offset) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, "config state=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset,
            appliance_console_state_string(snapshot->config_state)) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, " source=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset,
            appliance_console_source_string(snapshot->config_source))) {
        return APPLIANCE_CONSOLE_OVERFLOW;
    }

    if (snapshot->config_state !=
        APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        if (!appliance_console_append_cstr(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, " hostname=") ||
            !appliance_console_append_cstr(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, snapshot->config_hostname)) {
            return APPLIANCE_CONSOLE_OVERFLOW;
        }
    }
    if (!appliance_console_append_newline(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX, &offset) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, "health state=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset,
            appliance_console_state_string(snapshot->health_state))) {
        return APPLIANCE_CONSOLE_OVERFLOW;
    }
    if (snapshot->health_state !=
        APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        if (!appliance_console_append_cstr(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, " uptime_ticks=") ||
            !appliance_console_append_u64(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, snapshot->uptime_ticks)) {
            return APPLIANCE_CONSOLE_OVERFLOW;
        }
    }
    if (!appliance_console_append_newline(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX, &offset) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, "network state=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset,
            appliance_console_state_string(snapshot->network_state))) {
        return APPLIANCE_CONSOLE_OVERFLOW;
    }
    if (snapshot->network_state !=
        APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        if (!appliance_console_append_cstr(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, " generation=") ||
            !appliance_console_append_u64(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, snapshot->network_generation) ||
            !appliance_console_append_cstr(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, " devices=") ||
            !appliance_console_append_u64(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, snapshot->network_devices)) {
            return APPLIANCE_CONSOLE_OVERFLOW;
        }
    }
    if (!appliance_console_append_newline(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX, &offset) ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset, "services state=") ||
        !appliance_console_append_cstr(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
            &offset,
            appliance_console_state_string(snapshot->services_state))) {
        return APPLIANCE_CONSOLE_OVERFLOW;
    }
    if (snapshot->services_state !=
        APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        if (!appliance_console_append_cstr(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, " services=") ||
            !appliance_console_append_u64(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, snapshot->service_count) ||
            !appliance_console_append_cstr(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, " ready=") ||
            !appliance_console_append_u64(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, snapshot->service_ready) ||
            !appliance_console_append_cstr(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, " failed=") ||
            !appliance_console_append_u64(
                rendered, APPLIANCE_CONSOLE_OUTPUT_MAX,
                &offset, snapshot->service_failed)) {
            return APPLIANCE_CONSOLE_OVERFLOW;
        }
    }
    if (!appliance_console_append_newline(
            rendered, APPLIANCE_CONSOLE_OUTPUT_MAX, &offset)) {
        return APPLIANCE_CONSOLE_OVERFLOW;
    }

    rendered[offset] = '\0';
    if (capacity <= offset) {
        return APPLIANCE_CONSOLE_OVERFLOW;
    }
    for (index = 0U; index < offset; ++index) {
        output[index] = rendered[index];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_CONSOLE_OK;
}

const char *appliance_console_state_string(
    enum appliance_console_state state)
{
    switch (state) {
    case APPLIANCE_CONSOLE_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_CONSOLE_STATE_OK:
        return "OK";
    case APPLIANCE_CONSOLE_STATE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_CONSOLE_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_console_status_string(
    enum appliance_console_status status)
{
    switch (status) {
    case APPLIANCE_CONSOLE_OK:
        return "APPLIANCE_CONSOLE_OK";
    case APPLIANCE_CONSOLE_NULL:
        return "APPLIANCE_CONSOLE_NULL";
    case APPLIANCE_CONSOLE_BAD_VERSION:
        return "APPLIANCE_CONSOLE_BAD_VERSION";
    case APPLIANCE_CONSOLE_BAD_STATE:
        return "APPLIANCE_CONSOLE_BAD_STATE";
    case APPLIANCE_CONSOLE_BAD_IDENTITY:
        return "APPLIANCE_CONSOLE_BAD_IDENTITY";
    case APPLIANCE_CONSOLE_BAD_CONFIG:
        return "APPLIANCE_CONSOLE_BAD_CONFIG";
    case APPLIANCE_CONSOLE_BAD_HEALTH:
        return "APPLIANCE_CONSOLE_BAD_HEALTH";
    case APPLIANCE_CONSOLE_BAD_NETWORK:
        return "APPLIANCE_CONSOLE_BAD_NETWORK";
    case APPLIANCE_CONSOLE_BAD_SERVICE:
        return "APPLIANCE_CONSOLE_BAD_SERVICE";
    case APPLIANCE_CONSOLE_OVERFLOW:
        return "APPLIANCE_CONSOLE_OVERFLOW";
    default:
        return NULL;
    }
}
