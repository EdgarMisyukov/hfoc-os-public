#include "appliance_production_panel_view.h"

#include <string.h>

static bool appliance_production_panel_view_copy(
    char *destination,
    size_t capacity,
    const char *source)
{
    size_t length;

    if (destination == NULL || capacity == 0U || source == NULL) {
        return false;
    }
    length = strlen(source);
    if (length == 0U || length + 1U > capacity) {
        return false;
    }
    memcpy(destination, source, length + 1U);
    return true;
}

static bool appliance_production_panel_view_append(
    char *destination,
    size_t capacity,
    size_t *length,
    const char *text)
{
    const size_t text_length = strlen(text);

    if (text_length == 0U) {
        return true;
    }
    if (*length + text_length + 1U > capacity) {
        return false;
    }
    memcpy(destination + *length, text, text_length + 1U);
    *length += text_length;
    return true;
}

static bool appliance_production_panel_view_mac(
    char *destination,
    size_t capacity,
    const uint8_t *mac_address)
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    if (destination == NULL ||
        capacity < APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_MAX ||
        mac_address == NULL) {
        return false;
    }
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_LENGTH;
         ++index) {
        const size_t offset = index * 3U;

        destination[offset] = digits[(mac_address[index] >> 4U) & 0xFU];
        destination[offset + 1U] = digits[mac_address[index] & 0xFU];
        destination[offset + 2U] =
            index + 1U == APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_LENGTH
                ? '\0'
                : ':';
    }
    return true;
}

/*
 * The detail line is assembled into the storage and published only when it was
 * assembled whole: a partly built `state=... reason=` is left where it is and
 * the view names the field NULL, so the page says `unavailable` instead of
 * showing a half sentence.
 */
static void appliance_production_panel_view_detail(
    const struct appliance_production_panel_view_owners *owners,
    struct appliance_production_panel_view_storage *storage)
{
    size_t length = 0U;

    if (owners->serving_state == NULL || owners->serving_state[0] == '\0') {
        return;
    }
    if (!appliance_production_panel_view_append(
            storage->detail, sizeof(storage->detail), &length, "state=") ||
        !appliance_production_panel_view_append(
            storage->detail, sizeof(storage->detail), &length,
            owners->serving_state)) {
        return;
    }
    if (owners->serving_reason != NULL &&
        owners->serving_reason[0] != '\0') {
        if (!appliance_production_panel_view_append(
                storage->detail, sizeof(storage->detail), &length,
                " reason=") ||
            !appliance_production_panel_view_append(
                storage->detail, sizeof(storage->detail), &length,
                owners->serving_reason)) {
            return;
        }
    }
    storage->view.detail = storage->detail;
}

enum appliance_production_panel_view_status
appliance_production_panel_view_compose(
    const struct appliance_production_panel_view_owners *owners,
    struct appliance_production_panel_view_storage *storage)
{
    if (storage == NULL) {
        return APPLIANCE_PRODUCTION_PANEL_VIEW_NULL;
    }
    memset(storage, 0, sizeof(*storage));
    if (owners == NULL) {
        return APPLIANCE_PRODUCTION_PANEL_VIEW_NULL;
    }
    if (owners->version != APPLIANCE_PRODUCTION_PANEL_VIEW_VERSION) {
        return APPLIANCE_PRODUCTION_PANEL_VIEW_BAD_VERSION;
    }

    if (appliance_production_panel_view_copy(
            storage->hostname, sizeof(storage->hostname),
            owners->hostname)) {
        storage->view.hostname = storage->hostname;
    }
    if (appliance_production_panel_view_mac(
            storage->mac_address, sizeof(storage->mac_address),
            owners->mac_address)) {
        storage->view.mac_address = storage->mac_address;
    }
    if (appliance_production_panel_view_copy(
            storage->service_name, sizeof(storage->service_name),
            owners->service_name)) {
        storage->view.service_name = storage->service_name;
    }
    appliance_production_panel_view_detail(owners, storage);
    if (appliance_production_panel_view_copy(
            storage->log_line, sizeof(storage->log_line),
            owners->recent_log)) {
        storage->view.log_line = storage->log_line;
    }
    return APPLIANCE_PRODUCTION_PANEL_VIEW_OK;
}
