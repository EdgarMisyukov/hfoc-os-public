#include "local_network_setup.h"

#include <string.h>

/*
 * ITEM-447: the four questions the machine asks, and nothing else.
 *
 * The module holds one answer in progress, consumes the entered line, and
 * reports what the console has to do; the console owns the record and the
 * commit.  Every value is bounded, every refusal names its reason, and the
 * static answer is validated by the one IPv4 owner before the caller may
 * commit it.
 */

static bool local_network_setup_initialized(
    const struct local_network_setup *setup)
{
    return setup != NULL &&
           setup->version == LOCAL_NETWORK_SETUP_VERSION;
}

static size_t local_network_setup_text_length(const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return 0U;
    }
    while (length < LOCAL_NETWORK_SETUP_FIELD_MAX &&
           text[length] != '\0') {
        ++length;
    }
    return length;
}

static void local_network_setup_copy(
    char destination[LOCAL_NETWORK_SETUP_FIELD_MAX],
    const char *value,
    size_t value_length)
{
    size_t index;

    for (index = 0U; index < LOCAL_NETWORK_SETUP_FIELD_MAX; ++index) {
        destination[index] = '\0';
    }
    for (index = 0U;
         index + 1U < LOCAL_NETWORK_SETUP_FIELD_MAX &&
         index < value_length;
         ++index) {
        destination[index] = value[index];
    }
}

static void local_network_setup_write(
    char *output,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t index;

    if (output == NULL || length == NULL || text == NULL) {
        return;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (*length + 1U < capacity) {
            output[*length] = text[index];
            ++(*length);
        }
    }
    output[*length] = '\0';
}

void local_network_setup_begin(struct local_network_setup *setup)
{
    if (setup == NULL) {
        return;
    }
    memset(setup, 0, sizeof(*setup));
    setup->version = LOCAL_NETWORK_SETUP_VERSION;
    setup->active = true;
    setup->step = LOCAL_NETWORK_SETUP_STEP_MODE;
}

void local_network_setup_abort(struct local_network_setup *setup)
{
    if (setup == NULL) {
        return;
    }
    memset(setup, 0, sizeof(*setup));
    setup->version = LOCAL_NETWORK_SETUP_VERSION;
}

bool local_network_setup_active(const struct local_network_setup *setup)
{
    return local_network_setup_initialized(setup) && setup->active;
}

enum local_network_setup_step local_network_setup_step(
    const struct local_network_setup *setup)
{
    if (!local_network_setup_initialized(setup)) {
        return LOCAL_NETWORK_SETUP_STEP_IDLE;
    }
    return setup->step;
}

enum local_network_setup_status local_network_setup_prompt(
    const struct local_network_setup *setup,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t length = 0U;

    if (!local_network_setup_active(setup) || output == NULL ||
        output_length == NULL || capacity == 0U) {
        return LOCAL_NETWORK_SETUP_NULL;
    }
    output[0] = '\0';
    switch (setup->step) {
    case LOCAL_NETWORK_SETUP_STEP_MODE:
        local_network_setup_write(
            output, capacity, &length,
            "network mode [dhcp|static] (empty cancels): ");
        break;
    case LOCAL_NETWORK_SETUP_STEP_ADDRESS:
        local_network_setup_write(
            output, capacity, &length, "address a.b.c.d/prefix: ");
        break;
    case LOCAL_NETWORK_SETUP_STEP_GATEWAY:
        local_network_setup_write(output, capacity, &length, "gateway: ");
        break;
    case LOCAL_NETWORK_SETUP_STEP_DNS:
        local_network_setup_write(output, capacity, &length, "dns: ");
        break;
    default:
        return LOCAL_NETWORK_SETUP_BAD_STATE;
    }
    *output_length = length;
    return LOCAL_NETWORK_SETUP_OK;
}

static enum local_network_setup_status local_network_setup_advance(
    struct local_network_setup *setup,
    enum local_network_setup_action *action_out,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    enum local_network_setup_status status;

    setup->step = (enum local_network_setup_step)((uint32_t)setup->step + 1U);
    status = local_network_setup_prompt(
        setup, output, capacity, output_length);
    if (status != LOCAL_NETWORK_SETUP_OK) {
        return status;
    }
    *action_out = LOCAL_NETWORK_SETUP_ACTION_PROMPT;
    return LOCAL_NETWORK_SETUP_OK;
}

static bool local_network_setup_mode_is(
    const char *line,
    size_t line_length,
    const char *expected)
{
    const size_t expected_length =
        local_network_setup_text_length(expected);

    return line_length == expected_length &&
           (line_length == 0U ||
            memcmp(line, expected, line_length) == 0);
}

static enum local_network_setup_status local_network_setup_refuse(
    struct local_network_setup *setup,
    const char *reason,
    enum local_network_setup_action *action_out,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    local_network_setup_abort(setup);
    if (local_network_setup_refusal(
            reason, output, capacity, output_length) !=
        LOCAL_NETWORK_SETUP_OK) {
        return LOCAL_NETWORK_SETUP_BUFFER_TOO_SMALL;
    }
    *action_out = LOCAL_NETWORK_SETUP_ACTION_REFUSED;
    return LOCAL_NETWORK_SETUP_OK;
}

enum local_network_setup_status local_network_setup_feed(
    struct local_network_setup *setup,
    const char *line,
    size_t line_length,
    enum local_network_setup_action *action_out,
    char *output,
    size_t capacity,
    size_t *output_length,
    struct network_ipv4_config *config_out)
{
    if (!local_network_setup_active(setup) || action_out == NULL ||
        output == NULL || output_length == NULL || capacity == 0U) {
        return LOCAL_NETWORK_SETUP_NULL;
    }
    output[0] = '\0';
    *output_length = 0U;
    *action_out = LOCAL_NETWORK_SETUP_ACTION_NONE;
    if (line == NULL) {
        line_length = 0U;
    }
    if (line_length == 0U) {
        local_network_setup_abort(setup);
        *action_out = LOCAL_NETWORK_SETUP_ACTION_CANCEL;
        return LOCAL_NETWORK_SETUP_OK;
    }
    switch (setup->step) {
    case LOCAL_NETWORK_SETUP_STEP_MODE:
        if (local_network_setup_mode_is(line, line_length, "dhcp")) {
            local_network_setup_abort(setup);
            *action_out = LOCAL_NETWORK_SETUP_ACTION_COMMIT_DHCP;
            return LOCAL_NETWORK_SETUP_OK;
        }
        if (!local_network_setup_mode_is(line, line_length, "static")) {
            return local_network_setup_refuse(
                setup, "MODE", action_out, output, capacity, output_length);
        }
        setup->static_selected = true;
        return local_network_setup_advance(
            setup, action_out, output, capacity, output_length);
    case LOCAL_NETWORK_SETUP_STEP_ADDRESS:
        if (line_length >= LOCAL_NETWORK_SETUP_FIELD_MAX) {
            return local_network_setup_refuse(
                setup, "ADDRESS_TOO_LONG", action_out, output, capacity,
                output_length);
        }
        local_network_setup_copy(setup->address, line, line_length);
        return local_network_setup_advance(
            setup, action_out, output, capacity, output_length);
    case LOCAL_NETWORK_SETUP_STEP_GATEWAY:
        if (line_length >= LOCAL_NETWORK_SETUP_FIELD_MAX) {
            return local_network_setup_refuse(
                setup, "GATEWAY_TOO_LONG", action_out, output, capacity,
                output_length);
        }
        local_network_setup_copy(setup->gateway, line, line_length);
        return local_network_setup_advance(
            setup, action_out, output, capacity, output_length);
    case LOCAL_NETWORK_SETUP_STEP_DNS: {
        struct network_ipv4_config config;
        enum network_ipv4_status ipv4_status;

        if (config_out == NULL) {
            return LOCAL_NETWORK_SETUP_NULL;
        }
        if (line_length >= LOCAL_NETWORK_SETUP_FIELD_MAX) {
            return local_network_setup_refuse(
                setup, "DNS_TOO_LONG", action_out, output, capacity,
                output_length);
        }
        local_network_setup_copy(setup->dns, line, line_length);
        memset(&config, 0, sizeof(config));
        ipv4_status = network_ipv4_config_from_parts(
            setup->address,
            local_network_setup_text_length(setup->address),
            setup->gateway,
            local_network_setup_text_length(setup->gateway),
            setup->dns,
            local_network_setup_text_length(setup->dns),
            &config);
        if (ipv4_status != NETWORK_IPV4_OK) {
            const char *reason = network_ipv4_status_string(ipv4_status);

            return local_network_setup_refuse(
                setup,
                reason != NULL ? reason : "NETWORK_IPV4_MALFORMED_FIELD",
                action_out, output, capacity, output_length);
        }
        local_network_setup_abort(setup);
        *config_out = config;
        *action_out = LOCAL_NETWORK_SETUP_ACTION_COMMIT_STATIC;
        return LOCAL_NETWORK_SETUP_OK;
    }
    default:
        return LOCAL_NETWORK_SETUP_BAD_STATE;
    }
}

static void local_network_setup_write_u8(
    char *output,
    size_t capacity,
    size_t *length,
    uint8_t value)
{
    char digits[4U];
    size_t digit = 0U;

    do {
        digits[digit++] = (char)('0' + (value % 10U));
        value = (uint8_t)(value / 10U);
    } while (value != 0U && digit < sizeof(digits));
    while (digit > 0U) {
        char one[2U];

        one[0U] = digits[--digit];
        one[1U] = '\0';
        local_network_setup_write(output, capacity, length, one);
    }
}

static void local_network_setup_write_address(
    char *output,
    size_t capacity,
    size_t *length,
    const struct network_ipv4_address *address)
{
    uint32_t index;

    for (index = 0U; index < NETWORK_IPV4_ADDRESS_OCTETS; ++index) {
        if (index != 0U) {
            local_network_setup_write(output, capacity, length, ".");
        }
        local_network_setup_write_u8(
            output, capacity, length, address->octets[index]);
    }
}

/*
 * ITEM-496: the current configuration is the caller's rendering.  The module
 * only writes it into the record, and it never invents one: an empty or
 * missing rendering is published as UNAVAILABLE, because a record that named
 * a mode the interface does not have would be worse than a record that admits
 * it does not know.
 */
static void local_network_setup_write_current(
    char *output,
    size_t capacity,
    size_t *length,
    const char *current,
    size_t current_length)
{
    if (current == NULL || current_length == 0U) {
        local_network_setup_write(
            output, capacity, length, "UNAVAILABLE");
        return;
    }
    for (size_t index = 0U; index < current_length; ++index) {
        char one[2U];

        one[0U] = current[index];
        one[1U] = '\0';
        local_network_setup_write(output, capacity, length, one);
    }
}

enum local_network_setup_status local_network_setup_offer(
    const char *current,
    size_t current_length,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t length = 0U;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return LOCAL_NETWORK_SETUP_NULL;
    }
    output[0] = '\0';
    local_network_setup_write(
        output, capacity, &length,
        "[PRODUCTION-NET-SET] state=OFFERED current=");
    local_network_setup_write_current(
        output, capacity, &length, current, current_length);
    local_network_setup_write(output, capacity, &length, "\n");
    *output_length = length;
    return LOCAL_NETWORK_SETUP_OK;
}

enum local_network_setup_status local_network_setup_record(
    bool is_static,
    const struct network_ipv4_config *config,
    bool persisted,
    const char *current,
    size_t current_length,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t length = 0U;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return LOCAL_NETWORK_SETUP_NULL;
    }
    if (is_static && config == NULL) {
        return LOCAL_NETWORK_SETUP_NULL;
    }
    output[0] = '\0';
    local_network_setup_write(
        output, capacity, &length, "[PRODUCTION-NET-SET] mode=");
    local_network_setup_write(
        output, capacity, &length, is_static ? "STATIC" : "DHCP");
    if (is_static) {
        local_network_setup_write(output, capacity, &length, " address=");
        local_network_setup_write_address(
            output, capacity, &length, &config->address);
        local_network_setup_write(output, capacity, &length, "/");
        local_network_setup_write_u8(
            output, capacity, &length, config->prefix_length);
        local_network_setup_write(output, capacity, &length, " gateway=");
        local_network_setup_write_address(
            output, capacity, &length, &config->gateway);
        local_network_setup_write(output, capacity, &length, " dns=");
        local_network_setup_write_address(
            output, capacity, &length, &config->dns);
    }
    local_network_setup_write(output, capacity, &length, " persisted=");
    local_network_setup_write(
        output, capacity, &length, persisted ? "OK" : "FAILED");
    /*
     * ITEM-496: what was just stored and what the interface still runs are
     * two different facts until the reboot; both are named.
     */
    local_network_setup_write(
        output, capacity, &length, " applies=reboot current=");
    local_network_setup_write_current(
        output, capacity, &length, current, current_length);
    local_network_setup_write(output, capacity, &length, "\n");
    *output_length = length;
    return LOCAL_NETWORK_SETUP_OK;
}

enum local_network_setup_status local_network_setup_refusal(
    const char *reason,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t length = 0U;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return LOCAL_NETWORK_SETUP_NULL;
    }
    output[0] = '\0';
    local_network_setup_write(
        output, capacity, &length, "[PRODUCTION-NET-SET] state=REFUSED reason=");
    local_network_setup_write(
        output, capacity, &length,
        reason != NULL ? reason : "UNKNOWN");
    local_network_setup_write(output, capacity, &length, "\n");
    *output_length = length;
    return LOCAL_NETWORK_SETUP_OK;
}

enum local_network_setup_status local_network_setup_unavailable(
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t length = 0U;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return LOCAL_NETWORK_SETUP_NULL;
    }
    output[0] = '\0';
    local_network_setup_write(
        output, capacity, &length,
        "[PRODUCTION-NET-SET] state=UNAVAILABLE reason=NO_RECORD_OWNER\n");
    *output_length = length;
    return LOCAL_NETWORK_SETUP_OK;
}

const char *local_network_setup_status_string(
    enum local_network_setup_status status)
{
    switch (status) {
    case LOCAL_NETWORK_SETUP_OK:
        return "LOCAL_NETWORK_SETUP_OK";
    case LOCAL_NETWORK_SETUP_NULL:
        return "LOCAL_NETWORK_SETUP_NULL";
    case LOCAL_NETWORK_SETUP_BAD_STATE:
        return "LOCAL_NETWORK_SETUP_BAD_STATE";
    case LOCAL_NETWORK_SETUP_CANCELLED:
        return "LOCAL_NETWORK_SETUP_CANCELLED";
    case LOCAL_NETWORK_SETUP_BUFFER_TOO_SMALL:
        return "LOCAL_NETWORK_SETUP_BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
