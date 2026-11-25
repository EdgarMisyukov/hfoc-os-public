#ifndef HFOC_APPLIANCE_BOOT_CONSOLE_H
#define HFOC_APPLIANCE_BOOT_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_console.h"
#include "appliance_identity.h"
#include "appliance_language.h"

#define APPLIANCE_BOOT_CONSOLE_VERSION 1U
#define APPLIANCE_BOOT_CONSOLE_OUTPUT_MAX 1024U
#define APPLIANCE_BOOT_CONSOLE_URL_MAX 128U

enum appliance_boot_console_action {
    APPLIANCE_BOOT_CONSOLE_ACTION_NONE = 0,
    APPLIANCE_BOOT_CONSOLE_ACTION_STATUS = 1,
    APPLIANCE_BOOT_CONSOLE_ACTION_NETWORK = 2,
    APPLIANCE_BOOT_CONSOLE_ACTION_DIAGNOSTICS = 3,
    APPLIANCE_BOOT_CONSOLE_ACTION_REBOOT = 4,
    APPLIANCE_BOOT_CONSOLE_ACTION_SHUTDOWN = 5,
    APPLIANCE_BOOT_CONSOLE_ACTION_RECOVERY = 6,
};

enum appliance_boot_console_status {
    APPLIANCE_BOOT_CONSOLE_OK = 0,
    APPLIANCE_BOOT_CONSOLE_NULL = 1,
    APPLIANCE_BOOT_CONSOLE_BAD_STATE = 2,
    APPLIANCE_BOOT_CONSOLE_BAD_VERSION = 3,
    APPLIANCE_BOOT_CONSOLE_BAD_IDENTITY = 4,
    APPLIANCE_BOOT_CONSOLE_BAD_HARDWARE = 5,
    APPLIANCE_BOOT_CONSOLE_BAD_STATE_VALUE = 6,
    APPLIANCE_BOOT_CONSOLE_BAD_MANAGEMENT_URL = 7,
    APPLIANCE_BOOT_CONSOLE_OVERFLOW = 8,
};

struct appliance_boot_console_snapshot {
    uint32_t version;
    bool published;
    char product[APPLIANCE_IDENTITY_FIELD_MAX];
    char architecture[APPLIANCE_IDENTITY_FIELD_MAX];
    char platform[APPLIANCE_IDENTITY_FIELD_MAX];
    char hostname[APPLIANCE_IDENTITY_FIELD_MAX];
    enum appliance_console_state health_state;
    enum appliance_console_state network_state;
    enum appliance_console_state services_state;
    uint32_t cpu_cores;
    uint64_t ram_bytes;
    bool storage_ready;
    uint32_t disk_count;
    uint64_t uptime_ticks;
    bool management_ready;
    char management_url[
        APPLIANCE_BOOT_CONSOLE_URL_MAX + 1U];
};

enum appliance_boot_console_status
appliance_boot_console_snapshot_init(
    struct appliance_boot_console_snapshot *snapshot,
    const struct appliance_identity *identity,
    enum appliance_console_state health_state,
    uint64_t uptime_ticks,
    uint32_t cpu_cores,
    uint64_t ram_bytes);
enum appliance_boot_console_status
appliance_boot_console_publish_storage(
    struct appliance_boot_console_snapshot *snapshot,
    bool storage_ready,
    uint32_t disk_count);
enum appliance_boot_console_status
appliance_boot_console_publish_network(
    struct appliance_boot_console_snapshot *snapshot,
    enum appliance_console_state state,
    uint32_t device_count);
enum appliance_boot_console_status
appliance_boot_console_publish_services(
    struct appliance_boot_console_snapshot *snapshot,
    enum appliance_console_state state,
    uint32_t service_count);
enum appliance_boot_console_status
appliance_boot_console_publish_management(
    struct appliance_boot_console_snapshot *snapshot,
    const char *url,
    size_t url_length);
/*
 * ITEM-509 increment 9: the screen speaks the language the operator chose.
 * `language` is an explicit parameter for the same reason as in the local
 * menu: the caller cannot forget it, and the host proof renders both languages
 * without a machine.  The product name, the status values (OK, UNAVAILABLE)
 * and the machine words behind the entries stay English - the item's boundary
 * keeps records, markers and values as they are - and a catalog that cannot
 * translate the labels is refused instead of printing half a screen.
 */
enum appliance_boot_console_status
appliance_boot_console_render(
    const struct appliance_boot_console_snapshot *snapshot,
    enum appliance_language language,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_boot_console_action
appliance_boot_console_action(
    const char *command,
    size_t command_length);
const char *appliance_boot_console_action_string(
    enum appliance_boot_console_action action);
const char *appliance_boot_console_status_string(
    enum appliance_boot_console_status status);

#endif
