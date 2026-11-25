#include "appliance_dns_resolver.h"

#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_dns_resolver_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_dns_resolver_copy_bytes(
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

static size_t appliance_dns_resolver_string_length(
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool appliance_dns_resolver_valid_published(
    const struct appliance_dns_resolver *resolver)
{
    return resolver != NULL &&
           resolver->initialized &&
           resolver->version == APPLIANCE_DNS_RESOLVER_VERSION &&
           resolver->max_attempts == APPLIANCE_DNS_RESOLVER_MAX_ATTEMPTS;
}

static bool appliance_dns_resolver_state_valid(
    enum appliance_dns_resolver_state state)
{
    return state >= APPLIANCE_DNS_RESOLVER_STATE_NONE &&
           state <= APPLIANCE_DNS_RESOLVER_STATE_FAILED;
}

static bool appliance_dns_resolver_address_nonzero(
    const struct network_ipv4_address *address)
{
    size_t index;

    for (index = 0U;
         index < NETWORK_IPV4_ADDRESS_OCTETS;
         ++index) {
        if (address->octets[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool appliance_dns_resolver_interface_valid(
    const char *interface,
    size_t interface_length)
{
    size_t index;

    if (interface == NULL || interface_length == 0U ||
        interface_length > 15U) {
        return false;
    }
    if (interface[0] == '-' || interface[interface_length - 1U] == '-') {
        return false;
    }
    for (index = 0U; index < interface_length; ++index) {
        const unsigned char value = (unsigned char)interface[index];

        if (!((value >= (unsigned char)'a' &&
               value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' &&
               value <= (unsigned char)'9') ||
              value == (unsigned char)'-')) {
            return false;
        }
    }
    return true;
}

static bool appliance_dns_resolver_hostname_valid(
    const char *hostname,
    size_t hostname_length)
{
    size_t position = 0U;
    size_t label_length = 0U;
    size_t labels = 0U;

    if (hostname == NULL || hostname_length == 0U ||
        hostname_length > APPLIANCE_DNS_RESOLVER_HOSTNAME_MAX ||
        hostname[0] == '.' || hostname[0] == '-' ||
        hostname[hostname_length - 1U] == '.' ||
        hostname[hostname_length - 1U] == '-') {
        return false;
    }
    while (position < hostname_length) {
        const unsigned char value = (unsigned char)hostname[position];

        if (value == (unsigned char)'.') {
            if (label_length == 0U) {
                return false;
            }
            if (label_length > 63U) {
                return false;
            }
            label_length = 0U;
            ++labels;
        } else {
            if (!((value >= (unsigned char)'a' &&
                   value <= (unsigned char)'z') ||
                  (value >= (unsigned char)'A' &&
                   value <= (unsigned char)'Z') ||
                  (value >= (unsigned char)'0' &&
                   value <= (unsigned char)'9') ||
                  value == (unsigned char)'-')) {
                return false;
            }
            ++label_length;
            if (label_length > 63U) {
                return false;
            }
        }
        ++position;
    }
    if (label_length == 0U) {
        return false;
    }
    ++labels;
    return labels <= 127U;
}

static void appliance_dns_resolver_clear_pending(
    struct appliance_dns_resolver *resolver)
{
    appliance_dns_resolver_zero_bytes(
        &resolver->hostname, sizeof(resolver->hostname));
    resolver->hostname_length = 0U;
    appliance_dns_resolver_zero_bytes(
        &resolver->answer, sizeof(resolver->answer));
    resolver->answer_published = false;
    resolver->attempt_count = 0U;
}

static enum appliance_dns_resolver_status
appliance_dns_resolver_fail(
    struct appliance_dns_resolver *resolver)
{
    appliance_dns_resolver_clear_pending(resolver);
    resolver->state = APPLIANCE_DNS_RESOLVER_STATE_FAILED;
    return APPLIANCE_DNS_RESOLVER_EXHAUSTED;
}

enum appliance_dns_resolver_status
appliance_dns_resolver_init(
    struct appliance_dns_resolver *resolver,
    const struct network_ipv4_config *active_config,
    const char *interface,
    size_t interface_length)
{
    if (resolver == NULL || active_config == NULL ||
        interface == NULL) {
        return APPLIANCE_DNS_RESOLVER_NULL;
    }
    if (network_ipv4_config_validate(active_config) !=
            NETWORK_IPV4_OK ||
        !appliance_dns_resolver_address_nonzero(
            &active_config->dns)) {
        return APPLIANCE_DNS_RESOLVER_BAD_CONFIG;
    }
    if (!appliance_dns_resolver_interface_valid(
            interface, interface_length)) {
        return APPLIANCE_DNS_RESOLVER_BAD_CONFIG;
    }

    appliance_dns_resolver_zero_bytes(resolver, sizeof(*resolver));
    appliance_dns_resolver_copy_bytes(
        resolver->interface, interface, interface_length);
    resolver->interface_length = (uint8_t)interface_length;
    appliance_dns_resolver_copy_bytes(
        &resolver->server, &active_config->dns,
        sizeof(resolver->server));
    resolver->version = APPLIANCE_DNS_RESOLVER_VERSION;
    resolver->state = APPLIANCE_DNS_RESOLVER_STATE_READY;
    resolver->max_attempts = APPLIANCE_DNS_RESOLVER_MAX_ATTEMPTS;
    resolver->initialized = true;
    return APPLIANCE_DNS_RESOLVER_OK;
}

enum appliance_dns_resolver_status
appliance_dns_resolver_query(
    struct appliance_dns_resolver *resolver,
    const char *hostname,
    size_t hostname_length)
{
    if (resolver == NULL || hostname == NULL) {
        return APPLIANCE_DNS_RESOLVER_NULL;
    }
    if (!appliance_dns_resolver_valid_published(resolver)) {
        return APPLIANCE_DNS_RESOLVER_BAD_STATE;
    }
    if (resolver->state != APPLIANCE_DNS_RESOLVER_STATE_READY &&
        resolver->state != APPLIANCE_DNS_RESOLVER_STATE_RESOLVED) {
        return APPLIANCE_DNS_RESOLVER_STOPPED;
    }
    if (!appliance_dns_resolver_hostname_valid(
            hostname, hostname_length)) {
        return APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME;
    }

    appliance_dns_resolver_clear_pending(resolver);
    appliance_dns_resolver_copy_bytes(
        resolver->hostname, hostname, hostname_length);
    resolver->hostname_length = hostname_length;
    resolver->state = APPLIANCE_DNS_RESOLVER_STATE_QUERYING;
    resolver->attempt_count = 1U;
    return APPLIANCE_DNS_RESOLVER_OK;
}

enum appliance_dns_resolver_status
appliance_dns_resolver_response(
    struct appliance_dns_resolver *resolver,
    const struct network_ipv4_address *answer)
{
    if (resolver == NULL || answer == NULL) {
        return APPLIANCE_DNS_RESOLVER_NULL;
    }
    if (!appliance_dns_resolver_valid_published(resolver)) {
        return APPLIANCE_DNS_RESOLVER_BAD_STATE;
    }
    if (resolver->state != APPLIANCE_DNS_RESOLVER_STATE_QUERYING) {
        return APPLIANCE_DNS_RESOLVER_STOPPED;
    }
    if (!appliance_dns_resolver_address_nonzero(answer)) {
        return APPLIANCE_DNS_RESOLVER_BAD_ANSWER;
    }

    appliance_dns_resolver_copy_bytes(
        &resolver->answer, answer, sizeof(resolver->answer));
    resolver->answer_published = true;
    resolver->state = APPLIANCE_DNS_RESOLVER_STATE_RESOLVED;
    return APPLIANCE_DNS_RESOLVER_OK;
}

static enum appliance_dns_resolver_status
appliance_dns_resolver_retry(
    struct appliance_dns_resolver *resolver)
{
    if (resolver == NULL) {
        return APPLIANCE_DNS_RESOLVER_NULL;
    }
    if (!appliance_dns_resolver_valid_published(resolver)) {
        return APPLIANCE_DNS_RESOLVER_BAD_STATE;
    }
    if (resolver->state != APPLIANCE_DNS_RESOLVER_STATE_QUERYING) {
        return APPLIANCE_DNS_RESOLVER_STOPPED;
    }

    ++resolver->attempt_count;
    if (resolver->attempt_count >= resolver->max_attempts) {
        return appliance_dns_resolver_fail(resolver);
    }
    return APPLIANCE_DNS_RESOLVER_OK;
}

enum appliance_dns_resolver_status
appliance_dns_resolver_timeout(
    struct appliance_dns_resolver *resolver)
{
    return appliance_dns_resolver_retry(resolver);
}

enum appliance_dns_resolver_status
appliance_dns_resolver_server_failure(
    struct appliance_dns_resolver *resolver)
{
    return appliance_dns_resolver_retry(resolver);
}

enum appliance_dns_resolver_status
appliance_dns_resolver_clear(
    struct appliance_dns_resolver *resolver)
{
    if (resolver == NULL) {
        return APPLIANCE_DNS_RESOLVER_NULL;
    }
    if (!appliance_dns_resolver_valid_published(resolver)) {
        return APPLIANCE_DNS_RESOLVER_BAD_STATE;
    }

    appliance_dns_resolver_clear_pending(resolver);
    resolver->state = APPLIANCE_DNS_RESOLVER_STATE_READY;
    return APPLIANCE_DNS_RESOLVER_OK;
}

enum appliance_dns_resolver_status
appliance_dns_resolver_reset(
    struct appliance_dns_resolver *resolver)
{
    if (resolver == NULL) {
        return APPLIANCE_DNS_RESOLVER_NULL;
    }
    appliance_dns_resolver_zero_bytes(resolver, sizeof(*resolver));
    return APPLIANCE_DNS_RESOLVER_OK;
}

static bool appliance_dns_resolver_append_bytes(
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

static bool appliance_dns_resolver_append_string(
    char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    return appliance_dns_resolver_append_bytes(
        output, capacity, position,
        text, appliance_dns_resolver_string_length(text));
}

static bool appliance_dns_resolver_append_u32(
    char *output,
    size_t capacity,
    size_t *position,
    uint32_t value)
{
    char digits[10];
    unsigned int count = 0U;
    unsigned int index;

    if (value == 0U) {
        digits[count++] = '0';
    } else {
        while (value != 0U) {
            digits[count++] = (char)('0' + (value % 10U));
            value /= 10U;
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

static bool appliance_dns_resolver_append_address(
    char *output,
    size_t capacity,
    size_t *position,
    const struct network_ipv4_address *address)
{
    size_t index;

    for (index = 0U; index < NETWORK_IPV4_ADDRESS_OCTETS; ++index) {
        if (index != 0U &&
            !appliance_dns_resolver_append_string(
                output, capacity, position, ".")) {
            return false;
        }
        if (!appliance_dns_resolver_append_u32(
                output, capacity, position,
                address->octets[index])) {
            return false;
        }
    }
    return true;
}

enum appliance_dns_resolver_status
appliance_dns_resolver_render(
    const struct appliance_dns_resolver *resolver,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_DNS_RESOLVER_OUTPUT_MAX + 1U];
    size_t position = 0U;
    size_t index;

    if (resolver == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_DNS_RESOLVER_NULL;
    }
    if (!appliance_dns_resolver_valid_published(resolver)) {
        return APPLIANCE_DNS_RESOLVER_BAD_STATE;
    }

    if (!appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position,
            "dns_resolver v=") ||
        !appliance_dns_resolver_append_u32(
            rendered, sizeof(rendered), &position,
            APPLIANCE_DNS_RESOLVER_VERSION) ||
        !appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position, " interface=") ||
        !appliance_dns_resolver_append_bytes(
            rendered, sizeof(rendered), &position,
            resolver->interface, resolver->interface_length) ||
        !appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position, " server=") ||
        !appliance_dns_resolver_append_address(
            rendered, sizeof(rendered), &position, &resolver->server) ||
        !appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position, " host=") ||
        !appliance_dns_resolver_append_bytes(
            rendered, sizeof(rendered), &position,
            resolver->hostname, resolver->hostname_length) ||
        !appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position, " state=") ||
        !appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position,
            appliance_dns_resolver_state_string(resolver->state)) ||
        !appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position,
            resolver->answer_published ? " result=" : " result=none")) {
        return APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL;
    }
    if (resolver->answer_published) {
        if (!appliance_dns_resolver_append_address(
                rendered, sizeof(rendered), &position,
                &resolver->answer)) {
            return APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL;
        }
    }
    if (!appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position, " attempts=") ||
        !appliance_dns_resolver_append_u32(
            rendered, sizeof(rendered), &position,
            resolver->attempt_count)) {
        return APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL;
    }
    if (!appliance_dns_resolver_append_string(
            rendered, sizeof(rendered), &position, "\n")) {
        return APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL;
    }

    if (capacity <= position) {
        return APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL;
    }
    for (index = 0U; index < position; ++index) {
        output[index] = rendered[index];
    }
    output[position] = '\0';
    *output_length = position;
    return APPLIANCE_DNS_RESOLVER_OK;
}

enum appliance_dns_resolver_state
appliance_dns_resolver_state(
    const struct appliance_dns_resolver *resolver)
{
    if (!appliance_dns_resolver_valid_published(resolver)) {
        return APPLIANCE_DNS_RESOLVER_STATE_NONE;
    }
    return resolver->state;
}

bool appliance_dns_resolver_answer_published(
    const struct appliance_dns_resolver *resolver)
{
    return appliance_dns_resolver_valid_published(resolver) &&
           resolver->answer_published;
}

const struct network_ipv4_address *
appliance_dns_resolver_answer(
    const struct appliance_dns_resolver *resolver)
{
    if (!appliance_dns_resolver_valid_published(resolver) ||
        !resolver->answer_published) {
        return NULL;
    }
    return &resolver->answer;
}

const char *appliance_dns_resolver_state_string(
    enum appliance_dns_resolver_state state)
{
    if (!appliance_dns_resolver_state_valid(state)) {
        return NULL;
    }
    switch (state) {
    case APPLIANCE_DNS_RESOLVER_STATE_NONE:
        return "NONE";
    case APPLIANCE_DNS_RESOLVER_STATE_IDLE:
        return "IDLE";
    case APPLIANCE_DNS_RESOLVER_STATE_READY:
        return "READY";
    case APPLIANCE_DNS_RESOLVER_STATE_QUERYING:
        return "QUERYING";
    case APPLIANCE_DNS_RESOLVER_STATE_RESOLVED:
        return "RESOLVED";
    case APPLIANCE_DNS_RESOLVER_STATE_FAILED:
        return "FAILED";
    }
    return NULL;
}

const char *appliance_dns_resolver_status_string(
    enum appliance_dns_resolver_status status)
{
    if (status > APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL) {
        return NULL;
    }
    switch (status) {
    case APPLIANCE_DNS_RESOLVER_OK:
        return "APPLIANCE_DNS_RESOLVER_OK";
    case APPLIANCE_DNS_RESOLVER_NULL:
        return "APPLIANCE_DNS_RESOLVER_NULL";
    case APPLIANCE_DNS_RESOLVER_BAD_STATE:
        return "APPLIANCE_DNS_RESOLVER_BAD_STATE";
    case APPLIANCE_DNS_RESOLVER_BAD_VERSION:
        return "APPLIANCE_DNS_RESOLVER_BAD_VERSION";
    case APPLIANCE_DNS_RESOLVER_BAD_CONFIG:
        return "APPLIANCE_DNS_RESOLVER_BAD_CONFIG";
    case APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME:
        return "APPLIANCE_DNS_RESOLVER_BAD_HOSTNAME";
    case APPLIANCE_DNS_RESOLVER_BAD_ANSWER:
        return "APPLIANCE_DNS_RESOLVER_BAD_ANSWER";
    case APPLIANCE_DNS_RESOLVER_STOPPED:
        return "APPLIANCE_DNS_RESOLVER_STOPPED";
    case APPLIANCE_DNS_RESOLVER_EXHAUSTED:
        return "APPLIANCE_DNS_RESOLVER_EXHAUSTED";
    case APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL:
        return "APPLIANCE_DNS_RESOLVER_BUFFER_TOO_SMALL";
    }
    return NULL;
}
