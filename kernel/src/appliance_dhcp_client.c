#include "appliance_dhcp_client.h"

#include "appliance_network_config.h"
#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_dhcp_client_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_dhcp_client_copy_bytes(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *destination_bytes = destination;
    const unsigned char *source_bytes = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination_bytes[index] = source_bytes[index];
    }
}

static size_t appliance_dhcp_client_string_length(
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool appliance_dhcp_client_valid_published(
    const struct appliance_dhcp_client *client)
{
    return client != NULL &&
           client->initialized &&
           client->version == APPLIANCE_DHCP_CLIENT_VERSION &&
           client->max_attempts == APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS;
}

static bool appliance_dhcp_client_state_valid(
    enum appliance_dhcp_client_state state)
{
    return state >= APPLIANCE_DHCP_CLIENT_STATE_NONE &&
           state <= APPLIANCE_DHCP_CLIENT_STATE_FAILED;
}

static void appliance_dhcp_client_clear_lease(
    struct appliance_dhcp_client *client)
{
    appliance_dhcp_client_zero_bytes(
        &client->lease, sizeof(client->lease));
    client->lease_published = false;
}

static enum appliance_dhcp_client_status
appliance_dhcp_client_fail(
    struct appliance_dhcp_client *client)
{
    appliance_dhcp_client_clear_lease(client);
    client->state = APPLIANCE_DHCP_CLIENT_STATE_FAILED;
    return APPLIANCE_DHCP_CLIENT_EXHAUSTED;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_init(
    struct appliance_dhcp_client *client,
    const struct appliance_network_config *desired)
{
    if (client == NULL || desired == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (appliance_network_config_validate(desired) !=
            APPLIANCE_NETWORK_CONFIG_OK ||
        desired->mode != APPLIANCE_NETWORK_CONFIG_MODE_DHCP) {
        return APPLIANCE_DHCP_CLIENT_BAD_CONFIG;
    }

    appliance_dhcp_client_zero_bytes(client, sizeof(*client));
    appliance_dhcp_client_copy_bytes(
        &client->desired, desired, sizeof(client->desired));
    client->version = APPLIANCE_DHCP_CLIENT_VERSION;
    client->state = APPLIANCE_DHCP_CLIENT_STATE_IDLE;
    client->max_attempts = APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS;
    client->initialized = true;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_start(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_IDLE) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }

    appliance_dhcp_client_clear_lease(client);
    client->state = APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING;
    client->attempt_count = 1U;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_offer(
    struct appliance_dhcp_client *client,
    const struct network_ipv4_config *candidate)
{
    if (client == NULL || candidate == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }
    if (network_ipv4_config_validate(candidate) != NETWORK_IPV4_OK) {
        return APPLIANCE_DHCP_CLIENT_BAD_LEASE;
    }

    client->state = APPLIANCE_DHCP_CLIENT_STATE_REQUESTING;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_ack(
    struct appliance_dhcp_client *client,
    const struct network_ipv4_config *lease)
{
    if (client == NULL || lease == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_REQUESTING &&
        client->state != APPLIANCE_DHCP_CLIENT_STATE_RENEWING &&
        client->state != APPLIANCE_DHCP_CLIENT_STATE_REBINDING) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }
    if (network_ipv4_config_validate(lease) != NETWORK_IPV4_OK) {
        return APPLIANCE_DHCP_CLIENT_BAD_LEASE;
    }

    appliance_dhcp_client_copy_bytes(
        &client->lease, lease, sizeof(client->lease));
    client->lease_published = true;
    client->state = APPLIANCE_DHCP_CLIENT_STATE_BOUND;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_nak(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_REQUESTING &&
        client->state != APPLIANCE_DHCP_CLIENT_STATE_RENEWING &&
        client->state != APPLIANCE_DHCP_CLIENT_STATE_REBINDING) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }

    ++client->attempt_count;
    if (client->attempt_count >= client->max_attempts) {
        return appliance_dhcp_client_fail(client);
    }
    appliance_dhcp_client_clear_lease(client);
    client->state = APPLIANCE_DHCP_CLIENT_STATE_IDLE;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_discover_timeout(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }

    ++client->attempt_count;
    if (client->attempt_count >= client->max_attempts) {
        return appliance_dhcp_client_fail(client);
    }
    client->state = APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_request_timeout(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_REQUESTING) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }

    ++client->attempt_count;
    if (client->attempt_count >= client->max_attempts) {
        return appliance_dhcp_client_fail(client);
    }
    client->state = APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_renew(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_BOUND ||
        !client->lease_published) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }

    client->state = APPLIANCE_DHCP_CLIENT_STATE_RENEWING;
    client->attempt_count = 1U;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_renew_timeout(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_RENEWING) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }

    ++client->attempt_count;
    if (client->attempt_count >= client->max_attempts) {
        return appliance_dhcp_client_fail(client);
    }
    client->state = APPLIANCE_DHCP_CLIENT_STATE_REBINDING;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_rebind_timeout(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }
    if (client->state != APPLIANCE_DHCP_CLIENT_STATE_REBINDING) {
        return APPLIANCE_DHCP_CLIENT_STOPPED;
    }

    ++client->attempt_count;
    return appliance_dhcp_client_fail(client);
}

enum appliance_dhcp_client_status
appliance_dhcp_client_link_down(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }

    appliance_dhcp_client_clear_lease(client);
    client->state = APPLIANCE_DHCP_CLIENT_STATE_IDLE;
    client->attempt_count = 0U;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_reset(
    struct appliance_dhcp_client *client)
{
    if (client == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    appliance_dhcp_client_zero_bytes(client, sizeof(*client));
    return APPLIANCE_DHCP_CLIENT_OK;
}

static bool appliance_dhcp_client_append_bytes(
    char *output,
    size_t capacity,
    size_t *position,
    const void *source,
    size_t length)
{
    const unsigned char *bytes = source;
    size_t index;

    if (*position + length > capacity) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*position + index] = (char)bytes[index];
    }
    *position += length;
    return true;
}

static bool appliance_dhcp_client_append_string(
    char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    return appliance_dhcp_client_append_bytes(
        output, capacity, position,
        text, appliance_dhcp_client_string_length(text));
}

static bool appliance_dhcp_client_append_u8(
    char *output,
    size_t capacity,
    size_t *position,
    uint8_t value)
{
    char digits[3];
    unsigned int count = 0U;
    unsigned int index;

    if (value == 0U) {
        digits[count++] = '0';
    } else {
        while (value != 0U) {
            digits[count++] = (char)('0' + (value % 10U));
            value = (uint8_t)(value / 10U);
        }
    }
    for (index = 0U; index < count; ++index) {
        if (*position + 1U > capacity) {
            return false;
        }
        output[*position] = digits[count - index - 1U];
        ++(*position);
    }
    return true;
}

static bool appliance_dhcp_client_append_address(
    char *output,
    size_t capacity,
    size_t *position,
    const struct network_ipv4_address *address)
{
    size_t index;

    for (index = 0U; index < NETWORK_IPV4_ADDRESS_OCTETS; ++index) {
        if (index != 0U &&
            !appliance_dhcp_client_append_string(
                output, capacity, position, ".")) {
            return false;
        }
        if (!appliance_dhcp_client_append_u8(
                output, capacity, position, address->octets[index])) {
            return false;
        }
    }
    return true;
}

enum appliance_dhcp_client_status
appliance_dhcp_client_render(
    const struct appliance_dhcp_client *client,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_DHCP_CLIENT_OUTPUT_MAX + 1U];
    size_t position = 0U;
    size_t index;

    if (client == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_DHCP_CLIENT_NULL;
    }
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_BAD_STATE;
    }

    if (!appliance_dhcp_client_append_string(
            rendered, sizeof(rendered), &position, "dhcp_client v=") ||
        !appliance_dhcp_client_append_u8(
            rendered, sizeof(rendered), &position,
            APPLIANCE_DHCP_CLIENT_VERSION) ||
        !appliance_dhcp_client_append_string(
            rendered, sizeof(rendered), &position, " interface=") ||
        !appliance_dhcp_client_append_bytes(
            rendered, sizeof(rendered), &position,
            client->desired.interface,
            client->desired.interface_length) ||
        !appliance_dhcp_client_append_string(
            rendered, sizeof(rendered), &position, " state=") ||
        !appliance_dhcp_client_append_string(
            rendered, sizeof(rendered), &position,
            appliance_dhcp_client_state_string(client->state)) ||
        !appliance_dhcp_client_append_string(
            rendered, sizeof(rendered), &position,
            client->lease_published
                ? " lease=published"
                : " lease=none")) {
        return APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL;
    }
    if (client->lease_published) {
        const struct network_ipv4_config *lease = &client->lease;

        if (!appliance_dhcp_client_append_string(
                rendered, sizeof(rendered), &position, " address=") ||
            !appliance_dhcp_client_append_address(
                rendered, sizeof(rendered), &position, &lease->address) ||
            !appliance_dhcp_client_append_string(
                rendered, sizeof(rendered), &position, " prefix=") ||
            !appliance_dhcp_client_append_u8(
                rendered, sizeof(rendered), &position,
                lease->prefix_length) ||
            !appliance_dhcp_client_append_string(
                rendered, sizeof(rendered), &position, " gateway=") ||
            !appliance_dhcp_client_append_address(
                rendered, sizeof(rendered), &position, &lease->gateway) ||
            !appliance_dhcp_client_append_string(
                rendered, sizeof(rendered), &position, " dns=") ||
            !appliance_dhcp_client_append_address(
                rendered, sizeof(rendered), &position, &lease->dns)) {
            return APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL;
        }
    }
    if (!appliance_dhcp_client_append_string(
            rendered, sizeof(rendered), &position, "\n")) {
        return APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL;
    }

    if (capacity <= position) {
        return APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL;
    }
    for (index = 0U; index < position; ++index) {
        output[index] = rendered[index];
    }
    output[position] = '\0';
    *output_length = position;
    return APPLIANCE_DHCP_CLIENT_OK;
}

enum appliance_dhcp_client_state
appliance_dhcp_client_state(
    const struct appliance_dhcp_client *client)
{
    if (!appliance_dhcp_client_valid_published(client)) {
        return APPLIANCE_DHCP_CLIENT_STATE_NONE;
    }
    return client->state;
}

uint32_t appliance_dhcp_client_attempt_count(
    const struct appliance_dhcp_client *client)
{
    if (!appliance_dhcp_client_valid_published(client)) {
        return 0U;
    }
    return client->attempt_count;
}

uint32_t appliance_dhcp_client_attempt_budget(
    const struct appliance_dhcp_client *client)
{
    if (!appliance_dhcp_client_valid_published(client)) {
        return 0U;
    }
    return client->max_attempts;
}

bool appliance_dhcp_client_lease_published(
    const struct appliance_dhcp_client *client)
{
    return appliance_dhcp_client_valid_published(client) &&
           client->lease_published;
}

const struct network_ipv4_config *
appliance_dhcp_client_lease(
    const struct appliance_dhcp_client *client)
{
    if (!appliance_dhcp_client_valid_published(client) ||
        !client->lease_published) {
        return NULL;
    }
    return &client->lease;
}

const char *appliance_dhcp_client_state_string(
    enum appliance_dhcp_client_state state)
{
    if (!appliance_dhcp_client_state_valid(state)) {
        return NULL;
    }
    switch (state) {
    case APPLIANCE_DHCP_CLIENT_STATE_NONE:
        return "NONE";
    case APPLIANCE_DHCP_CLIENT_STATE_IDLE:
        return "IDLE";
    case APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING:
        return "DISCOVERING";
    case APPLIANCE_DHCP_CLIENT_STATE_REQUESTING:
        return "REQUESTING";
    case APPLIANCE_DHCP_CLIENT_STATE_BOUND:
        return "BOUND";
    case APPLIANCE_DHCP_CLIENT_STATE_RENEWING:
        return "RENEWING";
    case APPLIANCE_DHCP_CLIENT_STATE_REBINDING:
        return "REBINDING";
    case APPLIANCE_DHCP_CLIENT_STATE_FAILED:
        return "FAILED";
    }
    return NULL;
}

const char *appliance_dhcp_client_status_string(
    enum appliance_dhcp_client_status status)
{
    if (status > APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL) {
        return NULL;
    }
    switch (status) {
    case APPLIANCE_DHCP_CLIENT_OK:
        return "APPLIANCE_DHCP_CLIENT_OK";
    case APPLIANCE_DHCP_CLIENT_NULL:
        return "APPLIANCE_DHCP_CLIENT_NULL";
    case APPLIANCE_DHCP_CLIENT_BAD_STATE:
        return "APPLIANCE_DHCP_CLIENT_BAD_STATE";
    case APPLIANCE_DHCP_CLIENT_BAD_VERSION:
        return "APPLIANCE_DHCP_CLIENT_BAD_VERSION";
    case APPLIANCE_DHCP_CLIENT_BAD_CONFIG:
        return "APPLIANCE_DHCP_CLIENT_BAD_CONFIG";
    case APPLIANCE_DHCP_CLIENT_BAD_LEASE:
        return "APPLIANCE_DHCP_CLIENT_BAD_LEASE";
    case APPLIANCE_DHCP_CLIENT_STOPPED:
        return "APPLIANCE_DHCP_CLIENT_STOPPED";
    case APPLIANCE_DHCP_CLIENT_EXHAUSTED:
        return "APPLIANCE_DHCP_CLIENT_EXHAUSTED";
    case APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL:
        return "APPLIANCE_DHCP_CLIENT_BUFFER_TOO_SMALL";
    }
    return NULL;
}
