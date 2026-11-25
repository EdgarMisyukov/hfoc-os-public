#include "appliance_boot_console.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool appliance_boot_console_state_valid(
    enum appliance_console_state state)
{
    return appliance_console_state_string(state) != NULL;
}

static bool appliance_boot_console_copy_field(
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

static bool appliance_boot_console_snapshot_valid(
    const struct appliance_boot_console_snapshot *snapshot)
{
    if (snapshot == NULL ||
        !snapshot->published ||
        snapshot->version != APPLIANCE_BOOT_CONSOLE_VERSION ||
        snapshot->product[0] == '\0' ||
        snapshot->architecture[0] == '\0' ||
        snapshot->platform[0] == '\0' ||
        snapshot->hostname[0] == '\0' ||
        snapshot->cpu_cores == 0U ||
        snapshot->ram_bytes == 0U ||
        !appliance_boot_console_state_valid(
            snapshot->health_state) ||
        !appliance_boot_console_state_valid(
            snapshot->network_state) ||
        !appliance_boot_console_state_valid(
            snapshot->services_state)) {
        return false;
    }
    if (snapshot->storage_ready && snapshot->disk_count == 0U) {
        return false;
    }
    if (!snapshot->storage_ready && snapshot->disk_count != 0U) {
        return false;
    }
    if (snapshot->management_ready &&
        snapshot->management_url[0] == '\0') {
        return false;
    }
    if (!snapshot->management_ready &&
        snapshot->management_url[0] != '\0') {
        return false;
    }
    return true;
}

static bool appliance_boot_console_url_valid(
    const char *url,
    size_t url_length)
{
    static const char prefix[] = "https://";
    size_t index;

    if (url == NULL ||
        url_length <= sizeof(prefix) - 1U ||
        url_length > APPLIANCE_BOOT_CONSOLE_URL_MAX ||
        memcmp(url, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    for (index = sizeof(prefix) - 1U;
         index < url_length;
         ++index) {
        const unsigned char byte = (unsigned char)url[index];

        if (byte <= 0x20U || byte > 0x7EU ||
            byte == (unsigned char)'@' ||
            byte == (unsigned char)'\n' ||
            byte == (unsigned char)'\r') {
            return false;
        }
    }
    return true;
}

static bool appliance_boot_console_append_bytes(
    char *output,
    size_t capacity,
    size_t *position,
    const char *bytes,
    size_t length)
{
    size_t index;

    if (output == NULL ||
        *position > capacity ||
        length > capacity - *position) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*position + index] = bytes[index];
    }
    *position += length;
    return true;
}

static bool appliance_boot_console_append_string(
    char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return appliance_boot_console_append_bytes(
        output, capacity, position, text, length);
}

static bool appliance_boot_console_append_u64(
    char *output,
    size_t capacity,
    size_t *position,
    uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (value == 0U) {
        return appliance_boot_console_append_bytes(
            output, capacity, position, "0", 1U);
    }
    while (value != 0U) {
        digits[count] = (char)('0' + (value % 10U));
        ++count;
        value /= 10U;
    }
    while (count > 0U) {
        --count;
        if (!appliance_boot_console_append_bytes(
                output, capacity, position,
                &digits[count], 1U)) {
            return false;
        }
    }
    return true;
}

static bool appliance_boot_console_append_newline(
    char *output,
    size_t capacity,
    size_t *position)
{
    return appliance_boot_console_append_bytes(
        output, capacity, position, "\n", 1U);
}

enum appliance_boot_console_status
appliance_boot_console_snapshot_init(
    struct appliance_boot_console_snapshot *snapshot,
    const struct appliance_identity *identity,
    enum appliance_console_state health_state,
    uint64_t uptime_ticks,
    uint32_t cpu_cores,
    uint64_t ram_bytes)
{
    enum appliance_identity_status identity_status;

    if (snapshot == NULL || identity == NULL) {
        return APPLIANCE_BOOT_CONSOLE_NULL;
    }
    identity_status = appliance_identity_validate(identity);
    if (identity_status != APPLIANCE_IDENTITY_OK) {
        return APPLIANCE_BOOT_CONSOLE_BAD_IDENTITY;
    }
    if (!appliance_boot_console_state_valid(health_state) ||
        cpu_cores == 0U ||
        ram_bytes == 0U) {
        return APPLIANCE_BOOT_CONSOLE_BAD_HARDWARE;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (!appliance_boot_console_copy_field(
            snapshot->product, sizeof(snapshot->product),
            identity->product) ||
        !appliance_boot_console_copy_field(
            snapshot->architecture, sizeof(snapshot->architecture),
            identity->architecture) ||
        !appliance_boot_console_copy_field(
            snapshot->platform, sizeof(snapshot->platform),
            identity->platform) ||
        !appliance_boot_console_copy_field(
            snapshot->hostname, sizeof(snapshot->hostname),
            identity->hostname)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_IDENTITY;
    }

    snapshot->version = APPLIANCE_BOOT_CONSOLE_VERSION;
    snapshot->published = true;
    snapshot->health_state = health_state;
    snapshot->network_state = APPLIANCE_CONSOLE_STATE_UNAVAILABLE;
    snapshot->services_state = APPLIANCE_CONSOLE_STATE_UNAVAILABLE;
    snapshot->cpu_cores = cpu_cores;
    snapshot->ram_bytes = ram_bytes;
    snapshot->uptime_ticks = uptime_ticks;
    return APPLIANCE_BOOT_CONSOLE_OK;
}

enum appliance_boot_console_status
appliance_boot_console_publish_storage(
    struct appliance_boot_console_snapshot *snapshot,
    bool storage_ready,
    uint32_t disk_count)
{
    if (snapshot == NULL) {
        return APPLIANCE_BOOT_CONSOLE_NULL;
    }
    if (!appliance_boot_console_snapshot_valid(snapshot)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    if ((storage_ready && disk_count == 0U) ||
        (!storage_ready && disk_count != 0U)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    snapshot->storage_ready = storage_ready;
    snapshot->disk_count = disk_count;
    return APPLIANCE_BOOT_CONSOLE_OK;
}

enum appliance_boot_console_status
appliance_boot_console_publish_network(
    struct appliance_boot_console_snapshot *snapshot,
    enum appliance_console_state state,
    uint32_t device_count)
{
    if (snapshot == NULL) {
        return APPLIANCE_BOOT_CONSOLE_NULL;
    }
    if (!appliance_boot_console_snapshot_valid(snapshot)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    if (!appliance_boot_console_state_valid(state)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE_VALUE;
    }
    if (state == APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        if (device_count != 0U) {
            return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
        }
    } else if (device_count == 0U) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    snapshot->network_state = state;
    return APPLIANCE_BOOT_CONSOLE_OK;
}

enum appliance_boot_console_status
appliance_boot_console_publish_services(
    struct appliance_boot_console_snapshot *snapshot,
    enum appliance_console_state state,
    uint32_t service_count)
{
    if (snapshot == NULL) {
        return APPLIANCE_BOOT_CONSOLE_NULL;
    }
    if (!appliance_boot_console_snapshot_valid(snapshot)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    if (!appliance_boot_console_state_valid(state)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE_VALUE;
    }
    if (state == APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        if (service_count != 0U) {
            return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
        }
    } else if (service_count == 0U) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    snapshot->services_state = state;
    return APPLIANCE_BOOT_CONSOLE_OK;
}

enum appliance_boot_console_status
appliance_boot_console_publish_management(
    struct appliance_boot_console_snapshot *snapshot,
    const char *url,
    size_t url_length)
{
    size_t index;

    if (snapshot == NULL || url == NULL) {
        return APPLIANCE_BOOT_CONSOLE_NULL;
    }
    if (!appliance_boot_console_snapshot_valid(snapshot)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    if (!appliance_boot_console_url_valid(url, url_length)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_MANAGEMENT_URL;
    }
    if (snapshot->network_state ==
        APPLIANCE_CONSOLE_STATE_UNAVAILABLE) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    memset(snapshot->management_url, 0,
           sizeof(snapshot->management_url));
    for (index = 0U; index < url_length; ++index) {
        snapshot->management_url[index] = url[index];
    }
    snapshot->management_url[url_length] = '\0';
    snapshot->management_ready = true;
    return APPLIANCE_BOOT_CONSOLE_OK;
}

enum appliance_boot_console_status
appliance_boot_console_render(
    const struct appliance_boot_console_snapshot *snapshot,
    enum appliance_language language,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_BOOT_CONSOLE_OUTPUT_MAX];
    const char *health;
    const char *network;
    const char *services;
    size_t position = 0U;

    if (snapshot == NULL || output == NULL ||
        output_length == NULL) {
        return APPLIANCE_BOOT_CONSOLE_NULL;
    }
    if (!appliance_boot_console_snapshot_valid(snapshot)) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE;
    }
    health = appliance_console_state_string(snapshot->health_state);
    network = appliance_console_state_string(snapshot->network_state);
    services = appliance_console_state_string(snapshot->services_state);
    if (health == NULL || network == NULL || services == NULL) {
        return APPLIANCE_BOOT_CONSOLE_BAD_STATE_VALUE;
    }

#define APPEND_TEXT(text) \
    if (!appliance_boot_console_append_string( \
            rendered, sizeof(rendered), &position, (text))) { \
        return APPLIANCE_BOOT_CONSOLE_OVERFLOW; \
    }
/*
 * ITEM-509 increment 9: a label is either the catalog's translation or a named
 * refusal.  The catalog proof renders every key in both languages, so the
 * refusal branch is unreachable from a build that passed its own suite; it is
 * here so a broken catalog cannot print a half screen.
 */
#define APPEND_CATALOG(key) \
    do { \
        const char *catalog_text = \
            appliance_language_string(language, (key)); \
        if (catalog_text == NULL) { \
            return APPLIANCE_BOOT_CONSOLE_BAD_STATE_VALUE; \
        } \
        APPEND_TEXT(catalog_text); \
    } while (0)
#define APPEND_U64(value) \
    if (!appliance_boot_console_append_u64( \
            rendered, sizeof(rendered), &position, (value))) { \
        return APPLIANCE_BOOT_CONSOLE_OVERFLOW; \
    }
#define APPEND_NL() \
    if (!appliance_boot_console_append_newline( \
            rendered, sizeof(rendered), &position)) { \
        return APPLIANCE_BOOT_CONSOLE_OVERFLOW; \
    }

    APPEND_TEXT("HFOC Appliance");
    APPEND_NL();
    APPEND_TEXT("--------------");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.hostname");
    APPEND_TEXT(snapshot->hostname);
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.cpu");
    APPEND_U64(snapshot->cpu_cores);
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.memory");
    APPEND_U64(snapshot->ram_bytes / (1024U * 1024U));
    APPEND_TEXT(" MiB");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.storage");
    APPEND_TEXT(snapshot->storage_ready ? "OK" : "UNAVAILABLE");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.health");
    APPEND_TEXT(health);
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.network");
    APPEND_TEXT(network);
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.services");
    APPEND_TEXT(services);
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.management");
    if (snapshot->management_ready) {
        APPEND_TEXT(snapshot->management_url);
    } else {
        APPEND_TEXT("UNAVAILABLE");
    }
    APPEND_NL();
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.local_admin");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.entry.status");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.entry.network");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.entry.diagnostics");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.entry.reboot");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.entry.shutdown");
    APPEND_NL();
    APPEND_CATALOG("menu.bootconsole.entry.recovery");
    APPEND_NL();

#undef APPEND_TEXT
#undef APPEND_CATALOG
#undef APPEND_U64
#undef APPEND_NL

    if (position >= capacity) {
        return APPLIANCE_BOOT_CONSOLE_OVERFLOW;
    }
    memcpy(output, rendered, position);
    output[position] = '\0';
    *output_length = position;
    return APPLIANCE_BOOT_CONSOLE_OK;
}

enum appliance_boot_console_action
appliance_boot_console_action(
    const char *command,
    size_t command_length)
{
    static const struct {
        const char *word;
        enum appliance_boot_console_action action;
    } actions[] = {
        {"status", APPLIANCE_BOOT_CONSOLE_ACTION_STATUS},
        {"network", APPLIANCE_BOOT_CONSOLE_ACTION_NETWORK},
        {"diag", APPLIANCE_BOOT_CONSOLE_ACTION_DIAGNOSTICS},
        {"diagnostics", APPLIANCE_BOOT_CONSOLE_ACTION_DIAGNOSTICS},
        {"reboot", APPLIANCE_BOOT_CONSOLE_ACTION_REBOOT},
        {"shutdown", APPLIANCE_BOOT_CONSOLE_ACTION_SHUTDOWN},
        {"recovery", APPLIANCE_BOOT_CONSOLE_ACTION_RECOVERY},
    };
    size_t index;

    if (command == NULL || command_length == 0U) {
        return APPLIANCE_BOOT_CONSOLE_ACTION_NONE;
    }
    for (index = 0U;
         index < sizeof(actions) / sizeof(actions[0]);
         ++index) {
        const size_t word_length = strlen(actions[index].word);

        if (command_length == word_length &&
            memcmp(command, actions[index].word, word_length) == 0) {
            return actions[index].action;
        }
    }
    return APPLIANCE_BOOT_CONSOLE_ACTION_NONE;
}

const char *appliance_boot_console_action_string(
    enum appliance_boot_console_action action)
{
    switch (action) {
    case APPLIANCE_BOOT_CONSOLE_ACTION_NONE:
        return "APPLIANCE_BOOT_CONSOLE_ACTION_NONE";
    case APPLIANCE_BOOT_CONSOLE_ACTION_STATUS:
        return "APPLIANCE_BOOT_CONSOLE_ACTION_STATUS";
    case APPLIANCE_BOOT_CONSOLE_ACTION_NETWORK:
        return "APPLIANCE_BOOT_CONSOLE_ACTION_NETWORK";
    case APPLIANCE_BOOT_CONSOLE_ACTION_DIAGNOSTICS:
        return "APPLIANCE_BOOT_CONSOLE_ACTION_DIAGNOSTICS";
    case APPLIANCE_BOOT_CONSOLE_ACTION_REBOOT:
        return "APPLIANCE_BOOT_CONSOLE_ACTION_REBOOT";
    case APPLIANCE_BOOT_CONSOLE_ACTION_SHUTDOWN:
        return "APPLIANCE_BOOT_CONSOLE_ACTION_SHUTDOWN";
    case APPLIANCE_BOOT_CONSOLE_ACTION_RECOVERY:
        return "APPLIANCE_BOOT_CONSOLE_ACTION_RECOVERY";
    default:
        return NULL;
    }
}

const char *appliance_boot_console_status_string(
    enum appliance_boot_console_status status)
{
    switch (status) {
    case APPLIANCE_BOOT_CONSOLE_OK:
        return "APPLIANCE_BOOT_CONSOLE_OK";
    case APPLIANCE_BOOT_CONSOLE_NULL:
        return "APPLIANCE_BOOT_CONSOLE_NULL";
    case APPLIANCE_BOOT_CONSOLE_BAD_STATE:
        return "APPLIANCE_BOOT_CONSOLE_BAD_STATE";
    case APPLIANCE_BOOT_CONSOLE_BAD_VERSION:
        return "APPLIANCE_BOOT_CONSOLE_BAD_VERSION";
    case APPLIANCE_BOOT_CONSOLE_BAD_IDENTITY:
        return "APPLIANCE_BOOT_CONSOLE_BAD_IDENTITY";
    case APPLIANCE_BOOT_CONSOLE_BAD_HARDWARE:
        return "APPLIANCE_BOOT_CONSOLE_BAD_HARDWARE";
    case APPLIANCE_BOOT_CONSOLE_BAD_STATE_VALUE:
        return "APPLIANCE_BOOT_CONSOLE_BAD_STATE_VALUE";
    case APPLIANCE_BOOT_CONSOLE_BAD_MANAGEMENT_URL:
        return "APPLIANCE_BOOT_CONSOLE_BAD_MANAGEMENT_URL";
    case APPLIANCE_BOOT_CONSOLE_OVERFLOW:
        return "APPLIANCE_BOOT_CONSOLE_OVERFLOW";
    default:
        return NULL;
    }
}
