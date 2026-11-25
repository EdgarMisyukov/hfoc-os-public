#include "production_install_media.h"

#include "appliance_language.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-394: bounded, allocation-free parser and renderer for the installer
 * medium's declaration.  Nothing here writes a sector or plans a layout; the
 * kernel-side flow (`production_install_flow.c`) owns the controller, the
 * identity guard and the publication sequence.
 */

#define PRODUCTION_INSTALL_MEDIA_MIB_LBA UINT64_C(2048)
/* The canonical IPv4 text one static declaration turns into. */
#define PRODUCTION_INSTALL_MEDIA_NETWORK_TEXT_MAX 256U

struct production_install_media_writer {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
};

static const char *production_install_media_layout_string(
    const struct production_install_media_declaration *declaration)
{
    if (!declaration->layout_declared) {
        return "default";
    }
    if (declaration->layout_update) {
        return PRODUCTION_INSTALL_MEDIA_LAYOUT_UPDATE;
    }
    return declaration->layout_preserve
               ? PRODUCTION_INSTALL_MEDIA_LAYOUT_PRESERVE
               : PRODUCTION_INSTALL_MEDIA_LAYOUT_WHOLE_DISK;
}

static bool production_install_media_token_valid(
    const char *value,
    size_t value_length)
{
    size_t index;

    if (value == NULL || value_length == 0U ||
        value_length >= PRODUCTION_INSTALL_MEDIA_TOKEN_MAX) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        const unsigned char byte = (unsigned char)value[index];

        if (byte <= 0x20U || byte > 0x7EU) {
            return false;
        }
    }
    return true;
}

/*
 * ITEM-654: a literal IPv4 unicast address.  The dotted-quad grammar belongs
 * to the one IPv4 owner (`network_ipv4_address_parse`); this helper only adds
 * the SNTP rule that the source has to be a single host: loopback,
 * unspecified, multicast and broadcast are not time servers.
 */
static bool production_install_media_parse_ipv4(
    const char *value,
    size_t value_length,
    struct network_ipv4_address *address_out)
{
    if (address_out == NULL ||
        network_ipv4_address_parse(value, value_length, address_out) !=
            NETWORK_IPV4_OK) {
        return false;
    }
    if (address_out->octets[0] == 0U ||
        address_out->octets[0] == 127U ||
        address_out->octets[0] >= 224U) {
        memset(address_out, 0, sizeof(*address_out));
        return false;
    }
    return true;
}

/*
 * ITEM-638: the forward client list shares the declaration's bounded text
 * rules but not the 32-byte single-token bound: four IPv4 addresses already
 * exceed it, and the access rule is the list.  The list keeps its own finite
 * 64-byte bound; the value is still validated by the forward owner later.
 */
static bool production_install_media_clients_valid(
    const char *value,
    size_t value_length)
{
    size_t index;

    if (value == NULL || value_length == 0U ||
        value_length >= PRODUCTION_INSTALL_MEDIA_FORWARD_CLIENTS_MAX) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        const unsigned char byte = (unsigned char)value[index];

        if (byte <= 0x20U || byte > 0x7EU) {
            return false;
        }
    }
    return true;
}

static void production_install_media_copy_value(
    char *destination,
    size_t capacity,
    const char *value,
    size_t value_length)
{
    size_t index;

    for (index = 0U; index < capacity; ++index) {
        destination[index] = '\0';
    }
    for (index = 0U; index + 1U < capacity && index < value_length; ++index) {
        destination[index] = value[index];
    }
}

static void production_install_media_copy_token(
    char destination[PRODUCTION_INSTALL_MEDIA_TOKEN_MAX],
    const char *value,
    size_t value_length)
{
    production_install_media_copy_value(destination,
                                        PRODUCTION_INSTALL_MEDIA_TOKEN_MAX,
                                        value, value_length);
}

static size_t production_install_media_text_length(const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return 0U;
    }
    while (length < PRODUCTION_INSTALL_MEDIA_RECORD_MAX &&
           text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool production_install_media_text_equal(
    const char *left,
    size_t left_length,
    const char *right)
{
    const size_t right_length =
        production_install_media_text_length(right);

    return left_length == right_length &&
           (left_length == 0U ||
            memcmp(left, right, left_length) == 0);
}

static bool production_install_media_text_starts_with(
    const char *text,
    size_t text_length,
    const char *prefix)
{
    const size_t prefix_length =
        production_install_media_text_length(prefix);

    return text_length >= prefix_length &&
           (prefix_length == 0U ||
            memcmp(text, prefix, prefix_length) == 0);
}

static void production_install_media_writer_append(
    struct production_install_media_writer *writer,
    const char *text)
{
    size_t index;

    if (writer->overflow || text == NULL) {
        writer->overflow = true;
        return;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (writer->length + 1U >= writer->capacity) {
            writer->overflow = true;
            return;
        }
        writer->buffer[writer->length++] = text[index];
    }
    writer->buffer[writer->length] = '\0';
}

static void production_install_media_writer_append_u64(
    struct production_install_media_writer *writer,
    uint64_t value)
{
    char digits[24U];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));

    while (count != 0U) {
        char one[2U];

        one[0U] = digits[--count];
        one[1U] = '\0';
        production_install_media_writer_append(writer, one);
    }
}

enum production_install_media_status production_install_media_mode_parse(
    const char *command_line,
    size_t command_line_length,
    enum production_install_media_mode *mode)
{
    size_t offset = 0U;
    bool declared = false;

    if (mode == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    *mode = PRODUCTION_INSTALL_MEDIA_MODE_NONE;
    if (command_line == NULL) {
        return command_line_length == 0U
                   ? PRODUCTION_INSTALL_MEDIA_OK
                   : PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    if (command_line_length > PRODUCTION_INSTALL_MEDIA_COMMAND_LINE_MAX) {
        return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }

    while (offset < command_line_length) {
        size_t start;
        size_t end;

        while (offset < command_line_length &&
               (command_line[offset] == ' ' ||
                command_line[offset] == '\t' ||
                command_line[offset] == '\r' ||
                command_line[offset] == '\n')) {
            ++offset;
        }
        start = offset;
        while (offset < command_line_length &&
               command_line[offset] != ' ' &&
               command_line[offset] != '\t' &&
               command_line[offset] != '\r' &&
               command_line[offset] != '\n') {
            ++offset;
        }
        end = offset;
        if (end == start) {
            continue;
        }
        if (production_install_media_text_equal(
                command_line + start, end - start,
                PRODUCTION_INSTALL_MEDIA_CMDLINE_NONE)) {
            *mode = PRODUCTION_INSTALL_MEDIA_MODE_NONE;
            declared = true;
        } else if (production_install_media_text_equal(
                       command_line + start, end - start,
                       PRODUCTION_INSTALL_MEDIA_CMDLINE_UNATTENDED)) {
            *mode = PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED;
            declared = true;
        } else if (production_install_media_text_equal(
                       command_line + start, end - start,
                       PRODUCTION_INSTALL_MEDIA_CMDLINE_CONSOLE)) {
            *mode = PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE;
            declared = true;
        } else if (production_install_media_text_starts_with(
                       command_line + start, end - start,
                       "hfoc.install=")) {
            /* An unknown install directive is refused instead of being
             * read as "no install requested". */
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
    }
    (void)declared;
    return PRODUCTION_INSTALL_MEDIA_OK;
}

bool production_install_media_management_declared(
    const char *command_line,
    size_t command_line_length,
    bool *management_interface)
{
    size_t offset = 0U;
    bool opt_in = false;
    bool local_only = false;

    if (management_interface != NULL) {
        *management_interface = false;
    }
    if (command_line == NULL) {
        return false;
    }
    while (offset < command_line_length) {
        size_t start;
        size_t end;

        while (offset < command_line_length &&
               (command_line[offset] == ' ' ||
                command_line[offset] == '\t' ||
                command_line[offset] == '\r' ||
                command_line[offset] == '\n')) {
            ++offset;
        }
        start = offset;
        while (offset < command_line_length &&
               command_line[offset] != ' ' &&
               command_line[offset] != '\t' &&
               command_line[offset] != '\r' &&
               command_line[offset] != '\n') {
            ++offset;
        }
        end = offset;
        if (end == start) {
            continue;
        }
        if (production_install_media_text_equal(
                command_line + start, end - start,
                PRODUCTION_INSTALL_MEDIA_CMDLINE_MANAGEMENT_INTERFACE)) {
            opt_in = true;
        } else if (production_install_media_text_equal(
                       command_line + start, end - start,
                       PRODUCTION_INSTALL_MEDIA_CMDLINE_MANAGEMENT_LOCAL)) {
            local_only = true;
        }
    }
    /*
     * An ambiguous declaration (both scopes, or neither) is not a declaration:
     * the caller then reports the scope as the kernel's default, which is what
     * it is.
     */
    if (opt_in == local_only) {
        return false;
    }
    if (management_interface != NULL) {
        *management_interface = opt_in;
    }
    return true;
}

/*
 * ITEM-446: the declaration into the configuration the IPv4 owner accepts.
 * ITEM-447 moved the `a.b.c.d/prefix` split into that owner
 * (`network_ipv4_config_from_parts`), so the medium declaration and the local
 * menu share one builder as well as one parser.
 */
enum production_install_media_status production_install_media_network_config(

    const struct production_install_media_declaration *declaration,
    bool *is_static_out,
    struct network_ipv4_config *config_out)
{
    enum network_ipv4_status ipv4_status;

    if (declaration == NULL || is_static_out == NULL || config_out == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    *is_static_out = false;
    memset(config_out, 0, sizeof(*config_out));
    /* ITEM-654: an explicit SNTP server is one unicast IPv4, independent of
     * the addressing mode.  A malformed value refuses the declaration before
     * the first write instead of being guessed or silently ignored. */
    if (declaration->sntp_declared) {
        struct network_ipv4_address sntp;

        if (!production_install_media_parse_ipv4(
                declaration->sntp, strlen(declaration->sntp), &sntp)) {
            return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
        }
    }
    if (!declaration->network_declared) {
        /*
         * The three values without a mode, or a mode that is not a mode, are
         * an incomplete declaration: refused instead of being read as DHCP.
         */
        if (declaration->address_declared ||
            declaration->gateway_declared ||
            declaration->dns_declared) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (!declaration->network_static) {
        if (declaration->address_declared ||
            declaration->gateway_declared ||
            declaration->dns_declared) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (!declaration->address_declared ||
        !declaration->gateway_declared ||
        !declaration->dns_declared) {
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    ipv4_status = network_ipv4_config_from_parts(
        declaration->address, strlen(declaration->address),
        declaration->gateway, strlen(declaration->gateway),
        declaration->dns, strlen(declaration->dns),
        config_out);
    if (ipv4_status != NETWORK_IPV4_OK) {
        memset(config_out, 0, sizeof(*config_out));
        return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    *is_static_out = true;
    return PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status production_install_media_network_boot(
    const char *command_line,
    size_t command_line_length,
    struct production_install_media_boot_network *network_out)
{
    size_t offset = 0U;
    bool mode_seen = false;

    if (network_out == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    memset(network_out, 0, sizeof(*network_out));
    if (command_line == NULL) {
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    while (offset < command_line_length) {
        size_t start;
        size_t end;

        while (offset < command_line_length &&
               (command_line[offset] == ' ' ||
                command_line[offset] == '\t' ||
                command_line[offset] == '\r' ||
                command_line[offset] == '\n')) {
            ++offset;
        }
        start = offset;
        while (offset < command_line_length &&
               command_line[offset] != ' ' &&
               command_line[offset] != '\t' &&
               command_line[offset] != '\r' &&
               command_line[offset] != '\n') {
            ++offset;
        }
        end = offset;
        if (end == start) {
            continue;
        }
        {
            const char *token = command_line + start;
            const size_t token_length = end - start;

            if (production_install_media_text_starts_with(
                    token, token_length,
                    PRODUCTION_INSTALL_MEDIA_CMDLINE_ADDRESS_PREFIX) ||
                production_install_media_text_starts_with(
                    token, token_length,
                    PRODUCTION_INSTALL_MEDIA_CMDLINE_GATEWAY_PREFIX) ||
                production_install_media_text_starts_with(
                    token, token_length,
                    PRODUCTION_INSTALL_MEDIA_CMDLINE_DNS_PREFIX)) {
                const bool is_address =
                    production_install_media_text_starts_with(
                        token, token_length,
                        PRODUCTION_INSTALL_MEDIA_CMDLINE_ADDRESS_PREFIX);
                const bool is_gateway =
                    production_install_media_text_starts_with(
                        token, token_length,
                        PRODUCTION_INSTALL_MEDIA_CMDLINE_GATEWAY_PREFIX);
                const char *prefix =
                    is_address
                        ? PRODUCTION_INSTALL_MEDIA_CMDLINE_ADDRESS_PREFIX
                        : (is_gateway
                               ? PRODUCTION_INSTALL_MEDIA_CMDLINE_GATEWAY_PREFIX
                               : PRODUCTION_INSTALL_MEDIA_CMDLINE_DNS_PREFIX);
                const size_t prefix_length =
                    production_install_media_text_length(prefix);
                char *destination =
                    is_address
                        ? network_out->address
                        : (is_gateway ? network_out->gateway
                                      : network_out->dns);

                if (!production_install_media_token_valid(
                        token + prefix_length,
                        token_length - prefix_length) ||
                    destination[0] != '\0') {
                    /*
                     * A duplicate or empty value is not resolved by guessing:
                     * the caller sees a malformed declaration.
                     */
                    return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
                }
                production_install_media_copy_token(
                    destination, token + prefix_length,
                    token_length - prefix_length);
                network_out->declared = true;
            } else if (production_install_media_text_starts_with(
                           token, token_length,
                           PRODUCTION_INSTALL_MEDIA_CMDLINE_SNTP_PREFIX)) {
                /*
                 * ITEM-654: a bad SNTP source is a diagnosis, not a network
                 * refusal.  The address is parsed independently; an empty,
                 * duplicate or non-unicast value is recorded as declared but
                 * invalid, and the caller publishes INVALID instead of
                 * silently falling back to the gateway.
                 */
                const size_t prefix_length =
                    production_install_media_text_length(
                        PRODUCTION_INSTALL_MEDIA_CMDLINE_SNTP_PREFIX);
                const char *value = token + prefix_length;
                const size_t value_length = token_length - prefix_length;

                if (network_out->sntp_declared) {
                    network_out->sntp_valid = false;
                } else {
                    network_out->sntp_declared = true;
                    network_out->sntp_valid =
                        production_install_media_parse_ipv4(
                            value, value_length,
                            &network_out->sntp_address);
                    if (network_out->sntp_valid) {
                        production_install_media_copy_token(
                            network_out->sntp, value, value_length);
                    }
                }
            } else if (production_install_media_text_equal(
                           token, token_length,
                           PRODUCTION_INSTALL_MEDIA_CMDLINE_NETWORK_DHCP)) {
                if (mode_seen) {
                    return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
                }
                mode_seen = true;
                network_out->declared = true;
                network_out->is_static = false;
            } else if (production_install_media_text_equal(
                           token, token_length,
                           PRODUCTION_INSTALL_MEDIA_CMDLINE_NETWORK_STATIC)) {
                if (mode_seen) {
                    return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
                }
                mode_seen = true;
                network_out->declared = true;
                network_out->is_static = true;
            } else if (production_install_media_text_starts_with(
                           token, token_length, "hfoc.network=")) {
                return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
            }
        }
    }
    if (network_out->declared && !mode_seen) {
        /* Address values without a mode are not a declaration. */
        memset(network_out, 0, sizeof(*network_out));
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    if (mode_seen && !network_out->is_static &&
        (network_out->address[0] != '\0' ||
         network_out->gateway[0] != '\0' ||
         network_out->dns[0] != '\0')) {
        memset(network_out, 0, sizeof(*network_out));
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    if (mode_seen && network_out->is_static) {
        struct production_install_media_declaration declaration;
        bool is_static = false;
        struct network_ipv4_config config;

        memset(&declaration, 0, sizeof(declaration));
        declaration.network_declared = true;
        declaration.network_static = true;
        declaration.address_declared = network_out->address[0] != '\0';
        declaration.gateway_declared = network_out->gateway[0] != '\0';
        declaration.dns_declared = network_out->dns[0] != '\0';
        production_install_media_copy_token(
            declaration.address,
            network_out->address,
            production_install_media_text_length(network_out->address));
        production_install_media_copy_token(
            declaration.gateway,
            network_out->gateway,
            production_install_media_text_length(network_out->gateway));
        production_install_media_copy_token(
            declaration.dns,
            network_out->dns,
            production_install_media_text_length(network_out->dns));
        memset(&config, 0, sizeof(config));
        if (production_install_media_network_config(
                &declaration, &is_static, &config) !=
                PRODUCTION_INSTALL_MEDIA_OK ||
            !is_static) {
            memset(network_out, 0, sizeof(*network_out));
            return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
        }
        network_out->config = config;
        network_out->config_valid = true;
    }
    return PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status production_install_media_network_cmdline(
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    struct production_install_media_writer writer;
    bool is_static = false;
    struct network_ipv4_config config;
    enum production_install_media_status status;

    if (declaration == NULL || buffer == NULL || length_out == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    *length_out = 0U;
    buffer[0] = '\0';
    memset(&config, 0, sizeof(config));
    status = production_install_media_network_config(
        declaration, &is_static, &config);
    if (status != PRODUCTION_INSTALL_MEDIA_OK) {
        return status;
    }
    if (!declaration->network_declared && !declaration->sntp_declared) {
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    memset(&writer, 0, sizeof(writer));
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    if (declaration->network_declared && !is_static) {
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_NETWORK_DHCP);
    } else if (declaration->network_declared) {
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_NETWORK_STATIC);
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_ADDRESS_PREFIX);
        production_install_media_writer_append(
            &writer, declaration->address);
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_GATEWAY_PREFIX);
        production_install_media_writer_append(
            &writer, declaration->gateway);
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_DNS_PREFIX);
        production_install_media_writer_append(
            &writer, declaration->dns);
    }
    if (declaration->sntp_declared) {
        if (writer.length != 0U) {
            production_install_media_writer_append(&writer, " ");
        }
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_SNTP_PREFIX);
        production_install_media_writer_append(
            &writer, declaration->sntp);
    }
    if (writer.overflow) {
        buffer[0] = '\0';
        return PRODUCTION_INSTALL_MEDIA_OVERFLOW;
    }
    *length_out = writer.length;
    return PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_forward_config(
    const struct production_install_media_declaration *declaration,
    struct forward_config *config_out)
{
    enum forward_config_status forward_status;
    bool has_values;

    if (declaration == NULL || config_out == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    has_values = declaration->forward_target[0] != '\0' ||
                 declaration->forward_target_port[0] != '\0' ||
                 declaration->forward_listen_port[0] != '\0' ||
                 declaration->forward_clients[0] != '\0';
    if (!declaration->forward_declared) {
        /*
         * Values without a mode are not a declaration: refused instead of
         * being read as "off with leftovers".
         */
        if (has_values) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        forward_status = forward_config_from_parts(
            false, false, NULL, 0U, NULL, 0U, NULL, 0U, NULL, 0U,
            config_out);
        return forward_status == FORWARD_CONFIG_OK
            ? PRODUCTION_INSTALL_MEDIA_OK
            : PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    if (!declaration->forward_enabled) {
        if (has_values) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        forward_status = forward_config_from_parts(
            true, false, NULL, 0U, NULL, 0U, NULL, 0U, NULL, 0U,
            config_out);
        return forward_status == FORWARD_CONFIG_OK
            ? PRODUCTION_INSTALL_MEDIA_OK
            : PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    if (!has_values ||
        declaration->forward_target[0] == '\0' ||
        declaration->forward_target_port[0] == '\0' ||
        declaration->forward_listen_port[0] == '\0' ||
        declaration->forward_clients[0] == '\0') {
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    forward_status = forward_config_from_parts(
        true, true,
        declaration->forward_target,
        production_install_media_text_length(
            declaration->forward_target),
        declaration->forward_target_port,
        production_install_media_text_length(
            declaration->forward_target_port),
        declaration->forward_listen_port,
        production_install_media_text_length(
            declaration->forward_listen_port),
        declaration->forward_clients,
        production_install_media_text_length(
            declaration->forward_clients),
        config_out);
    if (forward_status != FORWARD_CONFIG_OK) {
        memset(config_out, 0, sizeof(*config_out));
        return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    return PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status production_install_media_forward_boot(
    const char *command_line,
    size_t command_line_length,
    struct production_install_media_boot_forward *forward_out)
{
    size_t offset = 0U;
    bool mode_seen = false;

    if (forward_out == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    memset(forward_out, 0, sizeof(*forward_out));
    if (command_line == NULL) {
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    while (offset < command_line_length) {
        size_t start;
        size_t end;

        while (offset < command_line_length &&
               (command_line[offset] == ' ' ||
                command_line[offset] == '\t' ||
                command_line[offset] == '\r' ||
                command_line[offset] == '\n')) {
            ++offset;
        }
        start = offset;
        while (offset < command_line_length &&
               command_line[offset] != ' ' &&
               command_line[offset] != '\t' &&
               command_line[offset] != '\r' &&
               command_line[offset] != '\n') {
            ++offset;
        }
        end = offset;
        if (end == start) {
            continue;
        }
        {
            const char *token = command_line + start;
            const size_t token_length = end - start;

            if (production_install_media_text_starts_with(
                    token, token_length,
                    PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PREFIX) ||
                production_install_media_text_starts_with(
                    token, token_length,
                    PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PORT_PREFIX) ||
                production_install_media_text_starts_with(
                    token, token_length,
                    PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_LISTEN_PORT_PREFIX) ||
                production_install_media_text_starts_with(
                    token, token_length,
                    PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_CLIENTS_PREFIX)) {
                const bool is_target =
                    production_install_media_text_starts_with(
                        token, token_length,
                        PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PREFIX);
                const bool is_target_port =
                    production_install_media_text_starts_with(
                        token, token_length,
                        PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PORT_PREFIX);
                const bool is_listen_port =
                    production_install_media_text_starts_with(
                        token, token_length,
                        PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_LISTEN_PORT_PREFIX);
                const char *prefix =
                    is_target
                        ? PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PREFIX
                        : (is_target_port
                               ? PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PORT_PREFIX
                               : (is_listen_port
                                      ? PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_LISTEN_PORT_PREFIX
                                      : PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_CLIENTS_PREFIX));
                const size_t prefix_length =
                    production_install_media_text_length(prefix);
                char *destination =
                    is_target
                        ? forward_out->target
                        : (is_target_port
                               ? forward_out->target_port
                               : (is_listen_port
                                      ? forward_out->listen_port
                                      : forward_out->clients));
                const size_t destination_max =
                    is_target || is_target_port || is_listen_port
                        ? PRODUCTION_INSTALL_MEDIA_TOKEN_MAX
                        : PRODUCTION_INSTALL_MEDIA_FORWARD_CLIENTS_MAX;

                if (!production_install_media_token_valid(
                        token + prefix_length,
                        token_length - prefix_length) ||
                    token_length - prefix_length >= destination_max ||
                    destination[0] != '\0') {
                    /*
                     * A duplicate or empty value is not resolved by guessing:
                     * the caller sees a malformed declaration.
                     */
                    return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
                }
                production_install_media_copy_token(
                    destination, token + prefix_length,
                    token_length - prefix_length);
                forward_out->declared = true;
            } else if (production_install_media_text_equal(
                           token, token_length,
                           PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_ON)) {
                if (mode_seen) {
                    return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
                }
                mode_seen = true;
                forward_out->declared = true;
                forward_out->enabled = true;
            } else if (production_install_media_text_equal(
                           token, token_length,
                           PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_OFF)) {
                if (mode_seen) {
                    return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
                }
                mode_seen = true;
                forward_out->declared = true;
                forward_out->enabled = false;
            } else if (production_install_media_text_starts_with(
                           token, token_length, "hfoc.forward=")) {
                return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
            }
        }
    }
    if (forward_out->declared && !mode_seen) {
        /* Values without a mode are not a declaration. */
        memset(forward_out, 0, sizeof(*forward_out));
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    if (mode_seen && !forward_out->enabled &&
        (forward_out->target[0] != '\0' ||
         forward_out->target_port[0] != '\0' ||
         forward_out->listen_port[0] != '\0' ||
         forward_out->clients[0] != '\0')) {
        memset(forward_out, 0, sizeof(*forward_out));
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    if (mode_seen && forward_out->enabled) {
        enum forward_config_status forward_status;

        forward_status = forward_config_from_parts(
            true, true,
            forward_out->target,
            production_install_media_text_length(forward_out->target),
            forward_out->target_port,
            production_install_media_text_length(
                forward_out->target_port),
            forward_out->listen_port,
            production_install_media_text_length(
                forward_out->listen_port),
            forward_out->clients,
            production_install_media_text_length(forward_out->clients),
            &forward_out->config);
        if (forward_status != FORWARD_CONFIG_OK) {
            memset(forward_out, 0, sizeof(*forward_out));
            return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
        }
        forward_out->config_valid = true;
    }
    return PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status production_install_media_forward_cmdline(
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    struct production_install_media_writer writer;
    struct forward_config config;

    if (declaration == NULL || buffer == NULL || length_out == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    *length_out = 0U;
    buffer[0] = '\0';
    memset(&config, 0, sizeof(config));
    if (production_install_media_forward_config(declaration, &config) !=
        PRODUCTION_INSTALL_MEDIA_OK) {
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    if (!declaration->forward_declared) {
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    memset(&writer, 0, sizeof(writer));
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    if (!declaration->forward_enabled) {
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_OFF);
    } else {
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_ON);
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(
            &writer, PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PREFIX);
        production_install_media_writer_append(
            &writer, declaration->forward_target);
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(
            &writer,
            PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_TARGET_PORT_PREFIX);
        production_install_media_writer_append(
            &writer, declaration->forward_target_port);
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(
            &writer,
            PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_LISTEN_PORT_PREFIX);
        production_install_media_writer_append(
            &writer, declaration->forward_listen_port);
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(
            &writer,
            PRODUCTION_INSTALL_MEDIA_CMDLINE_FORWARD_CLIENTS_PREFIX);
        production_install_media_writer_append(
            &writer, declaration->forward_clients);
    }
    if (writer.overflow) {
        buffer[0] = '\0';
        return PRODUCTION_INSTALL_MEDIA_OVERFLOW;
    }
    *length_out = writer.length;
    return PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_installed_cmdline(
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    struct production_install_media_writer writer;
    char network[PRODUCTION_INSTALL_MEDIA_NETWORK_CMDLINE_MAX];
    char forward[PRODUCTION_INSTALL_MEDIA_FORWARD_CMDLINE_MAX];
    size_t network_length = 0U;
    size_t forward_length = 0U;
    enum production_install_media_status status;

    if (declaration == NULL || buffer == NULL || length_out == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    *length_out = 0U;
    network[0] = '\0';
    forward[0] = '\0';
    status = production_install_media_network_cmdline(
        declaration, network, sizeof(network), &network_length);
    if (status != PRODUCTION_INSTALL_MEDIA_OK) {
        buffer[0] = '\0';
        return status;
    }
    status = production_install_media_forward_cmdline(
        declaration, forward, sizeof(forward), &forward_length);
    if (status != PRODUCTION_INSTALL_MEDIA_OK) {
        buffer[0] = '\0';
        return status;
    }
    memset(&writer, 0, sizeof(writer));
    writer.buffer = buffer;
    writer.capacity = capacity;
    buffer[0] = '\0';
    production_install_media_writer_append(
        &writer,
        declaration->management_interface
            ? PRODUCTION_INSTALL_MEDIA_CMDLINE_MANAGEMENT_INTERFACE
            : PRODUCTION_INSTALL_MEDIA_CMDLINE_MANAGEMENT_LOCAL);
    if (network_length != 0U) {
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(&writer, network);
    }
    if (forward_length != 0U) {
        production_install_media_writer_append(&writer, " ");
        production_install_media_writer_append(&writer, forward);
    }
    if (writer.overflow) {
        buffer[0] = '\0';
        return PRODUCTION_INSTALL_MEDIA_OVERFLOW;
    }
    *length_out = writer.length;
    return PRODUCTION_INSTALL_MEDIA_OK;
}

bool production_install_media_management_opt_in(
    const char *command_line,
    size_t command_line_length)
{
    bool management_interface = false;

    return production_install_media_management_declared(
               command_line, command_line_length, &management_interface) &&
           management_interface;
}

static enum production_install_media_status
production_install_media_parse_line(
    const char *line,
    size_t line_length,
    struct production_install_media_declaration *declaration)
{
    const size_t confirm_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_CONFIRM_DIRECTIVE);
    const size_t target_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_TARGET_DIRECTIVE);
    const size_t management_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_MANAGEMENT_DIRECTIVE);
    const size_t layout_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_LAYOUT_DIRECTIVE);
    const size_t language_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_LANGUAGE_DIRECTIVE);
    const size_t host_seed_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_HOST_SEED_DIRECTIVE);
    const size_t network_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_NETWORK_DIRECTIVE);
    const size_t address_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_ADDRESS_DIRECTIVE);
    const size_t gateway_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_GATEWAY_DIRECTIVE);
    const size_t dns_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_DNS_DIRECTIVE);
    const size_t sntp_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_SNTP_DIRECTIVE);
    const size_t forward_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_FORWARD_DIRECTIVE);
    const size_t forward_target_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_FORWARD_TARGET_DIRECTIVE);
    const size_t forward_target_port_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_FORWARD_TARGET_PORT_DIRECTIVE);
    const size_t forward_listen_port_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_FORWARD_LISTEN_PORT_DIRECTIVE);
    const size_t forward_clients_length =
        production_install_media_text_length(
            PRODUCTION_INSTALL_MEDIA_FORWARD_CLIENTS_DIRECTIVE);

    if (line_length == 0U) {
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_CONFIRM_DIRECTIVE)) {
        const char *value = line + confirm_length;
        const size_t value_length = line_length - confirm_length;

        if (!production_install_media_token_valid(
                value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        declaration->confirm_declared = true;
        production_install_media_copy_token(
            declaration->confirm, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_TARGET_DIRECTIVE)) {
        const char *value = line + target_length;
        const size_t value_length = line_length - target_length;

        if (!production_install_media_token_valid(
                value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        declaration->target_declared = true;
        production_install_media_copy_token(
            declaration->target, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_LAYOUT_DIRECTIVE)) {
        const char *value = line + layout_length;
        const size_t value_length = line_length - layout_length;

        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_LAYOUT_WHOLE_DISK)) {
            declaration->present = true;
            declaration->layout_declared = true;
            declaration->layout_preserve = false;
            declaration->layout_update = false;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_LAYOUT_PRESERVE)) {
            declaration->present = true;
            declaration->layout_declared = true;
            declaration->layout_preserve = true;
            declaration->layout_update = false;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_LAYOUT_UPDATE)) {
            declaration->present = true;
            declaration->layout_declared = true;
            declaration->layout_preserve = false;
            declaration->layout_update = true;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        /* A layout the operator misspelled is refused, never guessed. */
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_LANGUAGE_DIRECTIVE)) {
        const char *value = line + language_length;
        const size_t value_length = line_length - language_length;

        /*
         * ITEM-509 increment 27 (509.7c-2): exactly `en` or `ru`, through the
         * one owner of the vocabulary.  A misspelled value refuses the whole
         * declaration with BAD_TEXT, exactly like an unknown layout or network
         * mode; a missing directive stays the default language (zero = `en`).
         */
        if (appliance_language_parse(value, value_length,
                                     &declaration->language) !=
            APPLIANCE_LANGUAGE_OK) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        declaration->language_declared = true;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_HOST_SEED_DIRECTIVE)) {
        const char *value = line + host_seed_length;
        const size_t value_length = line_length - host_seed_length;

        if (!production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_HOST_SEED_REQUIRED)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        declaration->host_seed_required = true;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_SSH_KEY_DIRECTIVE)) {
        const size_t directive_length =
            production_install_media_text_length(
                PRODUCTION_INSTALL_MEDIA_SSH_KEY_DIRECTIVE);
        const char *value = line + directive_length;
        const size_t value_length = line_length - directive_length;
        size_t index;

        /*
         * ITEM-606: exactly one `SHA256:<base64>` fingerprint, bounded and
         * printable.  An absent directive means the medium carries no key; a
         * duplicate is ambiguous and refuses the declaration.
         */
        if (declaration->ssh_key_declared ||
            value_length < 8U ||
            value_length >= sizeof(declaration->ssh_key_fingerprint) ||
            memcmp(value, "SHA256:", 7U) != 0) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        for (index = 7U; index < value_length; ++index) {
            const unsigned char byte = (unsigned char)value[index];

            if (!((byte >= 'A' && byte <= 'Z') ||
                  (byte >= 'a' && byte <= 'z') ||
                  (byte >= '0' && byte <= '9') ||
                  byte == '+' || byte == '/' || byte == '=')) {
                return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
            }
        }
        memset(declaration->ssh_key_fingerprint, 0,
               sizeof(declaration->ssh_key_fingerprint));
        memcpy(declaration->ssh_key_fingerprint, value, value_length);
        declaration->present = true;
        declaration->ssh_key_declared = true;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_NETWORK_DIRECTIVE)) {
        const char *value = line + network_length;
        const size_t value_length = line_length - network_length;

        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_NETWORK_DHCP)) {
            declaration->present = true;
            declaration->network_declared = true;
            declaration->network_static = false;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_NETWORK_STATIC)) {
            declaration->present = true;
            declaration->network_declared = true;
            declaration->network_static = true;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        /* A network mode the operator misspelled is refused, never guessed. */
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_ADDRESS_DIRECTIVE)) {
        const char *value = line + address_length;
        const size_t value_length = line_length - address_length;

        if (!production_install_media_token_valid(value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        declaration->address_declared = true;
        production_install_media_copy_token(
            declaration->address, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_GATEWAY_DIRECTIVE)) {
        const char *value = line + gateway_length;
        const size_t value_length = line_length - gateway_length;

        if (!production_install_media_token_valid(value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        declaration->gateway_declared = true;
        production_install_media_copy_token(
            declaration->gateway, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_DNS_DIRECTIVE)) {
        const char *value = line + dns_length;
        const size_t value_length = line_length - dns_length;

        if (!production_install_media_token_valid(value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        declaration->dns_declared = true;
        production_install_media_copy_token(
            declaration->dns, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_SNTP_DIRECTIVE)) {
        const char *value = line + sntp_length;
        const size_t value_length = line_length - sntp_length;

        /* ITEM-654: one literal unicast IPv4, validated by the same helper
         * the installed declaration and the boot entry use. */
        if (!production_install_media_parse_ipv4(
                value, value_length, &(struct network_ipv4_address){ 0 })) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        declaration->sntp_declared = true;
        production_install_media_copy_token(
            declaration->sntp, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_FORWARD_DIRECTIVE)) {
        const char *value = line + forward_length;
        const size_t value_length = line_length - forward_length;

        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_FORWARD_ON)) {
            declaration->present = true;
            declaration->forward_declared = true;
            declaration->forward_enabled = true;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_FORWARD_OFF)) {
            declaration->present = true;
            declaration->forward_declared = true;
            declaration->forward_enabled = false;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        /* A forward mode the operator misspelled is refused, never guessed. */
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_FORWARD_TARGET_DIRECTIVE)) {
        const char *value = line + forward_target_length;
        const size_t value_length = line_length - forward_target_length;

        if (!production_install_media_token_valid(value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        production_install_media_copy_token(
            declaration->forward_target, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_FORWARD_TARGET_PORT_DIRECTIVE)) {
        const char *value = line + forward_target_port_length;
        const size_t value_length =
            line_length - forward_target_port_length;

        if (!production_install_media_token_valid(value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        production_install_media_copy_token(
            declaration->forward_target_port, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_FORWARD_LISTEN_PORT_DIRECTIVE)) {
        const char *value = line + forward_listen_port_length;
        const size_t value_length =
            line_length - forward_listen_port_length;

        if (!production_install_media_token_valid(value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        production_install_media_copy_token(
            declaration->forward_listen_port, value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_FORWARD_CLIENTS_DIRECTIVE)) {
        const char *value = line + forward_clients_length;
        const size_t value_length = line_length - forward_clients_length;

        if (!production_install_media_clients_valid(value, value_length)) {
            return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
        }
        declaration->present = true;
        production_install_media_copy_value(
            declaration->forward_clients,
            sizeof(declaration->forward_clients), value, value_length);
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_text_starts_with(
            line, line_length,
            PRODUCTION_INSTALL_MEDIA_MANAGEMENT_DIRECTIVE)) {
        const char *value = line + management_length;
        const size_t value_length = line_length - management_length;

        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_MANAGEMENT_LOCAL)) {
            declaration->present = true;
            declaration->management_interface = false;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        if (production_install_media_text_equal(
                value, value_length,
                PRODUCTION_INSTALL_MEDIA_MANAGEMENT_INTERFACE)) {
            declaration->present = true;
            declaration->management_interface = true;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
}

enum production_install_media_status
production_install_media_declaration_parse(
    const char *text,
    size_t text_length,
    struct production_install_media_declaration *declaration)
{
    size_t offset = 0U;

    if (declaration == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    memset(declaration, 0, sizeof(*declaration));
    declaration->version = PRODUCTION_INSTALL_MEDIA_VERSION;
    if (text == NULL) {
        return text_length == 0U ? PRODUCTION_INSTALL_MEDIA_OK
                                 : PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    if (text_length > PRODUCTION_INSTALL_MEDIA_DECLARATION_MAX) {
        return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    if (text_length == 0U) {
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    /* `present` means "carries a directive": a medium whose declaration file
     * holds only comments carries no install declaration at all. */

    while (offset < text_length) {
        size_t end = offset;
        size_t start;
        enum production_install_media_status status;

        while (end < text_length && text[end] != '\n') {
            ++end;
        }
        start = offset;
        while (start < end && (text[start] == ' ' || text[start] == '\t')) {
            ++start;
        }
        while (end > start &&
               (text[end - 1U] == ' ' || text[end - 1U] == '\t' ||
                text[end - 1U] == '\r')) {
            --end;
        }
        if (start < end && text[start] != '#') {
            status = production_install_media_parse_line(
                text + start, end - start, declaration);
            if (status != PRODUCTION_INSTALL_MEDIA_OK) {
                return status;
            }
        }
        offset = end < text_length ? end + 1U : text_length;
        while (offset < text_length && text[offset] == '\n') {
            ++offset;
        }
    }
    return PRODUCTION_INSTALL_MEDIA_OK;
}

size_t production_install_media_candidate_count(
    const struct installer_discovery_report *report)
{
    size_t index;
    size_t count = 0U;

    if (report == NULL ||
        report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION) {
        return 0U;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        if (report->disks[index].present &&
            report->disks[index].id != 0U) {
            ++count;
        }
    }
    return count;
}

const struct installer_discovery_disk *production_install_media_candidate(
    const struct installer_discovery_report *report,
    size_t index)
{
    size_t offset;
    size_t seen = 0U;

    if (report == NULL ||
        report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION) {
        return NULL;
    }
    for (offset = 0U; offset < INSTALLER_DISCOVERY_DISK_CAPACITY; ++offset) {
        if (!report->disks[offset].present ||
            report->disks[offset].id == 0U) {
            continue;
        }
        if (seen == index) {
            return &report->disks[offset];
        }
        ++seen;
    }
    return NULL;
}

static bool production_install_media_report_usable(
    const struct installer_discovery_report *report)
{
    return report != NULL &&
           report->version == INSTALLER_HARDWARE_DISCOVERY_VERSION &&
           report->read_only &&
           report->scan_complete;
}

/*
 * ITEM-490: the disk the operator named on the storage screen becomes the disk
 * this declaration speaks about.  The rest of the declaration is untouched: the
 * confirmation token, the management scope, the layout, the network and the
 * forward are still the medium's words, and the identity has to be text the
 * plan can match against a disk the hardware reported.
 */
bool production_install_media_declaration_set_target(
    struct production_install_media_declaration *declaration,
    const char *identity)
{
    size_t identity_length;

    if (declaration == NULL || identity == NULL || identity[0] == '\0') {
        return false;
    }
    identity_length = production_install_media_text_length(identity);
    if (!production_install_media_token_valid(identity, identity_length)) {
        return false;
    }
    memset(declaration->target, 0, sizeof(declaration->target));
    production_install_media_copy_token(declaration->target, identity,
                                        identity_length);
    declaration->target_declared = true;
    return true;
}

enum production_install_media_status production_install_media_plan_build(
    enum production_install_media_mode mode,
    const struct production_install_media_declaration *declaration,
    enum installer_firmware_mode firmware_mode,
    const struct installer_discovery_report *report,
    struct production_install_media_plan *plan)
{
    size_t index;
    size_t match_count = 0U;
    const struct installer_discovery_disk *match = NULL;

    if (plan == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    if (mode != PRODUCTION_INSTALL_MEDIA_MODE_NONE &&
        mode != PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED &&
        mode != PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE) {
        return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    memset(plan, 0, sizeof(*plan));
    plan->version = PRODUCTION_INSTALL_MEDIA_VERSION;
    plan->mode = mode;
    if (mode == PRODUCTION_INSTALL_MEDIA_MODE_NONE) {
        plan->action = PRODUCTION_INSTALL_MEDIA_ACTION_NONE;
        plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_NOT_INSTALL_MEDIA;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }

    plan->action = PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED;
    if (declaration == NULL || !declaration->present) {
        plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MISSING;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (!declaration->confirm_declared) {
        plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_CONFIRM_MISSING;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (!declaration->target_declared) {
        plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_MISSING;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    /*
     * ITEM-446: the declared network is validated here, before the plan can
     * select a target and long before the first write.  A declaration that
     * cannot be honoured refuses the whole run with zero writes instead of
     * installing an appliance that would come up on a guessed address.
     */
    if (declaration->network_declared || declaration->address_declared ||
        declaration->gateway_declared || declaration->dns_declared) {
        struct network_ipv4_config network_config;
        bool network_static = false;
        enum production_install_media_status network_status;

        memset(&network_config, 0, sizeof(network_config));
        network_status = production_install_media_network_config(
            declaration, &network_static, &network_config);
        if (network_status != PRODUCTION_INSTALL_MEDIA_OK) {
            plan->refusal =
                network_status == PRODUCTION_INSTALL_MEDIA_BAD_TEXT
                    ? PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_INCOMPLETE
                    : PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_MALFORMED;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
    }
    /*
     * ITEM-449: the declared forward is validated here too, before the plan can
     * select a target and long before the first write.  A declaration that
     * cannot be honoured refuses the whole run with zero writes instead of
     * installing an appliance that would open a port nobody declared.
     */
    if (declaration->forward_declared ||
        declaration->forward_target[0] != '\0' ||
        declaration->forward_target_port[0] != '\0' ||
        declaration->forward_listen_port[0] != '\0' ||
        declaration->forward_clients[0] != '\0') {
        struct forward_config forward_config_value;
        enum production_install_media_status forward_status;

        memset(&forward_config_value, 0, sizeof(forward_config_value));
        forward_status = production_install_media_forward_config(
            declaration, &forward_config_value);
        if (forward_status != PRODUCTION_INSTALL_MEDIA_OK) {
            plan->refusal =
                forward_status == PRODUCTION_INSTALL_MEDIA_BAD_TEXT
                    ? PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_INCOMPLETE
                    : PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_MALFORMED;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
    }
    /*
     * ITEM-517: the firmware mode selects the installed boot chain; it does
     * not refuse the operator's declared target.  Only an unknown firmware is
     * refused, because no chain can be chosen for it.
     */
    if (firmware_mode == INSTALLER_FIRMWARE_UNKNOWN) {
        plan->refusal =
            PRODUCTION_INSTALL_MEDIA_REFUSAL_FIRMWARE_UNSUPPORTED;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (!production_install_media_report_usable(report)) {
        plan->refusal =
            PRODUCTION_INSTALL_MEDIA_REFUSAL_REPORT_UNAVAILABLE;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (production_install_media_candidate_count(report) == 0U) {
        plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_NO_DISK;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }

    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        const struct installer_discovery_disk *disk = &report->disks[index];
        size_t identity_length = 0U;

        if (!disk->present || disk->id == 0U) {
            continue;
        }
        while (identity_length < INSTALLER_DISCOVERY_IDENTITY_LENGTH &&
               disk->identity[identity_length] != '\0') {
            ++identity_length;
        }
        if (!production_install_media_text_equal(
                disk->identity, identity_length, declaration->target)) {
            continue;
        }
        match = disk;
        ++match_count;
    }
    if (match_count == 0U) {
        /*
         * ITEM-500: a medium declared for one machine is still a medium that
         * installs on another when a person is standing at the screen.  The
         * operator entry lists the disks this machine has and the operator
         * names the one to write, so the declared disk being absent is the
         * reason to ask - not a reason to refuse and leave the machine to boot
         * from the medium.  The unattended entry has nobody to ask and keeps
         * refusing with the same named reason.
         */
        if (mode == PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE) {
            plan->selected = false;
            plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE;
            plan->action = PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE;
            return PRODUCTION_INSTALL_MEDIA_OK;
        }
        plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_NOT_FOUND;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }
    if (match_count > 1U) {
        plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_AMBIGUOUS;
        return PRODUCTION_INSTALL_MEDIA_OK;
    }

    plan->selected = true;
    plan->refusal = PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE;
    plan->action = mode == PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED
                       ? PRODUCTION_INSTALL_MEDIA_ACTION_UNATTENDED
                       : PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE;
    plan->target.disk_id = match->id;
    plan->target.controller = match->controller;
    plan->target.block_count = match->block_count;
    production_install_media_copy_token(
        plan->target.identity, match->identity,
        production_install_media_text_length(match->identity));
    return PRODUCTION_INSTALL_MEDIA_OK;
}

bool production_install_media_confirm_matches(
    const struct production_install_media_declaration *declaration,
    const char *token,
    size_t token_length)
{
    if (declaration == NULL || !declaration->confirm_declared ||
        token == NULL || !production_install_media_token_valid(
                             token, token_length)) {
        return false;
    }
    return production_install_media_text_equal(
        token, token_length, declaration->confirm);
}

enum production_install_media_status
production_install_media_render_declaration(
    const struct production_install_media_declaration *declaration,
    enum production_install_media_mode mode,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (declaration == NULL || buffer == NULL || length == NULL ||
        capacity == 0U) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] media token=");
    production_install_media_writer_append(
        &writer, declaration->present ? "PRESENT" : "ABSENT");
    if (declaration->present) {
        production_install_media_writer_append(&writer, " confirm=");
        production_install_media_writer_append(
            &writer, declaration->confirm_declared
                         ? declaration->confirm
                         : "MISSING");
        production_install_media_writer_append(&writer, " target=");
        production_install_media_writer_append(
            &writer, declaration->target_declared
                         ? declaration->target
                         : "MISSING");
        production_install_media_writer_append(&writer, " management=");
        production_install_media_writer_append(
            &writer,
            production_install_media_management_string(declaration));
        /*
         * ITEM-398: the declared layout, named `mode=` like the flow's own
         * decision record.  It is not called `layout=` because the applied
         * record already ends in `layout=VERIFIED`, and one line must not carry
         * two different meanings under one key.
         */
        production_install_media_writer_append(&writer, " mode=");
        production_install_media_writer_append(
            &writer, production_install_media_layout_string(declaration));
        /*
         * ITEM-509 increment 27: the declared language is named in the record
         * only when the medium declares one.  A medium built before the
         * directive carries no fragment, so its record stays byte for byte
         * what it was; a declared language is read before the first write
         * instead of being discovered after the install.
         */
        if (declaration->language_declared) {
            production_install_media_writer_append(&writer, " language=");
            production_install_media_writer_append(
                &writer, appliance_language_code(declaration->language));
        }
        if (declaration->host_seed_required) {
            production_install_media_writer_append(
                &writer, " host_seed=required");
        }
        /*
         * ITEM-446: the declared network is part of the record the operator
         * reads before the first write.  A static declaration names the
         * address, the gateway and the DNS it carries into the installed boot
         * entry; `default` is DHCP, the behaviour without the directive.
         */
        production_install_media_writer_append(&writer, " network=");
        if (!declaration->network_declared) {
            production_install_media_writer_append(&writer, "default");
        } else if (!declaration->network_static) {
            production_install_media_writer_append(
                &writer, PRODUCTION_INSTALL_MEDIA_NETWORK_DHCP);
        } else {
            production_install_media_writer_append(
                &writer, PRODUCTION_INSTALL_MEDIA_NETWORK_STATIC);
            production_install_media_writer_append(&writer, " address=");
            production_install_media_writer_append(
                &writer,
                declaration->address_declared ? declaration->address
                                              : "MISSING");
            production_install_media_writer_append(&writer, " gateway=");
            production_install_media_writer_append(
                &writer,
                declaration->gateway_declared ? declaration->gateway
                                              : "MISSING");
            production_install_media_writer_append(&writer, " dns=");
            production_install_media_writer_append(
                &writer,
                declaration->dns_declared ? declaration->dns : "MISSING");
        }
        /*
         * ITEM-449: the declared forward is part of the record too.  It names
         * the target and the access rule the appliance will serve, or says
         * `off`, so the operator reads what the sealed medium declares before
         * the first write instead of discovering it after the install.
         */
        production_install_media_writer_append(&writer, " forward=");
        if (!declaration->forward_declared) {
            production_install_media_writer_append(&writer, "default");
        } else if (!declaration->forward_enabled) {
            production_install_media_writer_append(
                &writer, PRODUCTION_INSTALL_MEDIA_FORWARD_OFF);
        } else {
            production_install_media_writer_append(
                &writer, PRODUCTION_INSTALL_MEDIA_FORWARD_ON);
            production_install_media_writer_append(&writer, " target=");
            production_install_media_writer_append(
                &writer, declaration->forward_target);
            production_install_media_writer_append(&writer, " target-port=");
            production_install_media_writer_append(
                &writer, declaration->forward_target_port);
            production_install_media_writer_append(&writer, " listen-port=");
            production_install_media_writer_append(
                &writer, declaration->forward_listen_port);
            production_install_media_writer_append(&writer, " clients=");
            production_install_media_writer_append(
                &writer, declaration->forward_clients);
        }
    }
    production_install_media_writer_append(&writer, " entry=");
    production_install_media_writer_append(
        &writer, production_install_media_mode_string(mode));
    production_install_media_writer_append(&writer, "\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_candidate(
    const struct installer_discovery_disk *disk,
    const char *model,
    bool selected,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (disk == NULL || buffer == NULL || length == NULL ||
        capacity == 0U) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] candidate disk=");
    production_install_media_writer_append_u64(&writer, disk->id);
    production_install_media_writer_append(&writer, " controller=");
    production_install_media_writer_append(
        &writer,
        production_install_media_controller_string(disk->controller));
    production_install_media_writer_append(&writer, " identity=");
    production_install_media_writer_append(
        &writer, disk->identity[0] != '\0' ? disk->identity
                                           : "unknown");
    production_install_media_writer_append(&writer, " model=");
    production_install_media_writer_append(
        &writer, model != NULL && model[0] != '\0' ? model
                                                   : "unavailable");
    production_install_media_writer_append(&writer, " blocks=");
    production_install_media_writer_append_u64(
        &writer, disk->block_count);
    production_install_media_writer_append(&writer, " size_mib=");
    production_install_media_writer_append_u64(
        &writer,
        disk->block_size != 0U
            ? disk->block_count /
                  (UINT64_C(1048576) / disk->block_size)
            : 0U);
    production_install_media_writer_append(
        &writer, selected ? " selected=1\n" : " selected=0\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_confirmation(
    const struct production_install_media_declaration *declaration,
    enum production_install_media_mode mode,
    const struct production_install_media_target *target,
    enum production_install_media_confirmation confirmation,
    uint64_t uptime_seconds,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (declaration == NULL || target == NULL || buffer == NULL ||
        length == NULL || capacity == 0U) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] confirm source=");
    production_install_media_writer_append(
        &writer,
        confirmation == PRODUCTION_INSTALL_MEDIA_CONFIRMATION_OPERATOR
            ? "operator-screen"
            : "media-token");
    production_install_media_writer_append(&writer, " entry=");
    production_install_media_writer_append(
        &writer, production_install_media_mode_string(mode));
    production_install_media_writer_append(&writer, " token=");
    production_install_media_writer_append(&writer, declaration->confirm);
    production_install_media_writer_append(&writer, " target=");
    production_install_media_writer_append(&writer, declaration->target);
    production_install_media_writer_append(&writer, " disk=");
    production_install_media_writer_append_u64(&writer, target->disk_id);
    production_install_media_writer_append(&writer, " identity=");
    production_install_media_writer_append(&writer, target->identity);
    production_install_media_writer_append(&writer, " management=");
    production_install_media_writer_append(
        &writer,
        production_install_media_management_string(declaration));
    production_install_media_writer_append(&writer, " when_uptime_s=");
    production_install_media_writer_append_u64(&writer, uptime_seconds);
    production_install_media_writer_append(&writer, "\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_refusal(
    enum production_install_media_refusal refusal,
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (buffer == NULL || length == NULL || capacity == 0U) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    if (refusal == PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE) {
        return PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] refused reason=");
    production_install_media_writer_append(
        &writer, production_install_media_refusal_string(refusal));
    if (declaration != NULL && declaration->target_declared &&
        (refusal == PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_NOT_FOUND ||
         refusal == PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_AMBIGUOUS)) {
        production_install_media_writer_append(&writer, " target=");
        production_install_media_writer_append(
            &writer, declaration->target);
    }
    production_install_media_writer_append(&writer, " writes=0\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_applied(
    const struct production_install_media_declaration *declaration,
    const struct production_install_media_target *target,
    uint64_t uptime_seconds,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (target == NULL || buffer == NULL || length == NULL ||
        capacity == 0U) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] applied disk=");
    production_install_media_writer_append_u64(&writer, target->disk_id);
    production_install_media_writer_append(&writer, " identity=");
    production_install_media_writer_append(&writer, target->identity);
    production_install_media_writer_append(&writer, " controller=");
    production_install_media_writer_append(
        &writer,
        production_install_media_controller_string(target->controller));
    if (declaration != NULL) {
        production_install_media_writer_append(&writer, " management=");
        production_install_media_writer_append(
            &writer,
            production_install_media_management_string(declaration));
        production_install_media_writer_append(&writer, " mode=");
        production_install_media_writer_append(
            &writer, production_install_media_layout_string(declaration));
    }
    production_install_media_writer_append(&writer, " when_uptime_s=");
    production_install_media_writer_append_u64(&writer, uptime_seconds);
    production_install_media_writer_append(&writer, " layout=VERIFIED\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_choice_note(
    const struct production_install_media_declaration *declaration,
    enum appliance_language language,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;
    const char *prefix = appliance_language_string(
        language, "installer.media.declared_disk_prefix");
    const char *suffix = appliance_language_string(
        language, "installer.media.not_on_machine_suffix");

    if (declaration == NULL || buffer == NULL || length == NULL ||
        capacity == 0U) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    /*
     * ITEM-509 increment 15: a key the catalog cannot answer refuses the line
     * by name instead of publishing half of it; the declared target and the
     * `NONE` placeholder stay the machine values the medium carries.
     */
    if (prefix == NULL || suffix == NULL) {
        return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    /*
     * ITEM-500: the one line the storage screen shows above the disks when the
     * medium's declared disk is not on this machine.  The operator reads why
     * the machine is asking instead of installing, and which disk the medium
     * was built for.
     */
    production_install_media_writer_append(&writer, prefix);
    if (declaration->target_declared) {
        production_install_media_writer_append(&writer, declaration->target);
    } else {
        production_install_media_writer_append(&writer, "NONE");
    }
    production_install_media_writer_append(&writer, suffix);
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_awaiting_choice(
    const struct production_install_media_declaration *declaration,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (declaration == NULL || buffer == NULL || length == NULL ||
        capacity == 0U) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    /*
     * ITEM-500: the operator entry is prepared, and the disk it will write is
     * the one the operator names on the screen.  The record names the disk the
     * medium declared and says that this machine does not have it, so the run
     * is not mistaken for one that already has a target, and `disk=NONE` is
     * the honest reading of a screen nobody has chosen on yet.
     */
    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] pending source=console entry=console");
    production_install_media_writer_append(&writer, " token=");
    production_install_media_writer_append(&writer, declaration->confirm);
    production_install_media_writer_append(&writer, " target=");
    if (declaration->target_declared) {
        production_install_media_writer_append(&writer, declaration->target);
    } else {
        production_install_media_writer_append(&writer, "NONE");
    }
    production_install_media_writer_append(
        &writer, " declared=NOT_ON_THIS_MACHINE disk=NONE identity=NONE");
    production_install_media_writer_append(&writer, " writes=0\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_pending(
    const struct production_install_media_declaration *declaration,
    const struct production_install_media_target *target,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (declaration == NULL || target == NULL || buffer == NULL ||
        length == NULL || capacity == 0U) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] pending source=console entry=");
    production_install_media_writer_append(
        &writer,
        production_install_media_mode_string(
            PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE));
    production_install_media_writer_append(&writer, " token=");
    production_install_media_writer_append(&writer, declaration->confirm);
    production_install_media_writer_append(&writer, " target=");
    production_install_media_writer_append(&writer, declaration->target);
    production_install_media_writer_append(&writer, " disk=");
    production_install_media_writer_append_u64(&writer, target->disk_id);
    production_install_media_writer_append(&writer, " identity=");
    production_install_media_writer_append(&writer, target->identity);
    /*
     * ITEM-495: this record states what was prepared and that nothing has been
     * written yet.  It used to name the console command that confirmed it; the
     * operator entry now confirms on the screen, and the record that follows
     * (`confirm source=operator-screen`) is where that decision is written down.
     */
    production_install_media_writer_append(&writer, " writes=0\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_controller_refusal(
    const char *status_text,
    const char *detail_text,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (buffer == NULL || length == NULL || capacity == 0U ||
        status_text == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] refused reason=CONTROLLER status=");
    production_install_media_writer_append(&writer, status_text);
    if (detail_text != NULL && detail_text[0] != '\0') {
        production_install_media_writer_append(&writer, " detail=");
        production_install_media_writer_append(&writer, detail_text);
    }
    production_install_media_writer_append(&writer, " writes=0\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

enum production_install_media_status
production_install_media_render_failure(
    const char *stage,
    const char *status_text,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct production_install_media_writer writer;

    if (buffer == NULL || length == NULL || capacity == 0U ||
        stage == NULL || status_text == NULL) {
        return PRODUCTION_INSTALL_MEDIA_NULL;
    }
    writer.buffer = buffer;
    writer.capacity = capacity;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';

    production_install_media_writer_append(
        &writer, "[PRODUCTION-INSTALL] failed stage=");
    production_install_media_writer_append(&writer, stage);
    production_install_media_writer_append(&writer, " status=");
    production_install_media_writer_append(&writer, status_text);
    production_install_media_writer_append(&writer, "\n");
    *length = writer.length;
    return writer.overflow ? PRODUCTION_INSTALL_MEDIA_OVERFLOW
                           : PRODUCTION_INSTALL_MEDIA_OK;
}

const char *production_install_media_mode_string(
    enum production_install_media_mode mode)
{
    switch (mode) {
    case PRODUCTION_INSTALL_MEDIA_MODE_NONE:
        return "none";
    case PRODUCTION_INSTALL_MEDIA_MODE_UNATTENDED:
        return "unattended";
    case PRODUCTION_INSTALL_MEDIA_MODE_CONSOLE:
        return "console";
    default:
        return "unknown";
    }
}

const char *production_install_media_action_string(
    enum production_install_media_action action)
{
    switch (action) {
    case PRODUCTION_INSTALL_MEDIA_ACTION_NONE:
        return "NONE";
    case PRODUCTION_INSTALL_MEDIA_ACTION_UNATTENDED:
        return "UNATTENDED";
    case PRODUCTION_INSTALL_MEDIA_ACTION_CONSOLE:
        return "CONSOLE";
    case PRODUCTION_INSTALL_MEDIA_ACTION_REFUSED:
        return "REFUSED";
    default:
        return NULL;
    }
}

const char *production_install_media_refusal_string(
    enum production_install_media_refusal refusal)
{
    switch (refusal) {
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_NONE:
        return "NONE";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_NOT_INSTALL_MEDIA:
        return "NOT_INSTALL_MEDIA";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MISSING:
        return "TOKEN_MISSING";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_MALFORMED:
        return "TOKEN_MALFORMED";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_CONFIRM_MISSING:
        return "CONFIRM_MISSING";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_MISSING:
        return "TARGET_MISSING";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_FIRMWARE_UNSUPPORTED:
        return "FIRMWARE_UNSUPPORTED";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_REPORT_UNAVAILABLE:
        return "REPORT_UNAVAILABLE";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_NO_DISK:
        return "NO_DISK";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_NOT_FOUND:
        return "TARGET_NOT_FOUND";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_TARGET_AMBIGUOUS:
        return "TARGET_AMBIGUOUS";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_MODE_MALFORMED:
        return "MODE_MALFORMED";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_INCOMPLETE:
        return "NETWORK_INCOMPLETE";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_NETWORK_MALFORMED:
        return "NETWORK_MALFORMED";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_INCOMPLETE:
        return "FORWARD_INCOMPLETE";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_FORWARD_MALFORMED:
        return "FORWARD_MALFORMED";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_CANCELLED_BY_OPERATOR:
        return "CANCELLED_BY_OPERATOR";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_RELEASE_CONFLICT:
        return "RELEASE_CONFLICT";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_MISSING:
        return "HOST_SEED_MISSING";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_INVALID:
        return "HOST_SEED_INVALID";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_TARGET_MISMATCH:
        return "HOST_SEED_TARGET_MISMATCH";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_HOST_SEED_BUILD_MISMATCH:
        return "HOST_SEED_BUILD_MISMATCH";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_CREDENTIAL_REQUIRED:
        return "CREDENTIAL_REQUIRED";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_SSH_KEY_MISSING:
        return "SSH_KEY_MISSING";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_SSH_KEY_INVALID:
        return "SSH_KEY_INVALID";
    case PRODUCTION_INSTALL_MEDIA_REFUSAL_TOKEN_TOO_LARGE:
        return "TOKEN_TOO_LARGE";
    default:
        return NULL;
    }
}

const char *production_install_media_status_string(
    enum production_install_media_status status)
{
    switch (status) {
    case PRODUCTION_INSTALL_MEDIA_OK:
        return "PRODUCTION_INSTALL_MEDIA_OK";
    case PRODUCTION_INSTALL_MEDIA_NULL:
        return "PRODUCTION_INSTALL_MEDIA_NULL";
    case PRODUCTION_INSTALL_MEDIA_BAD_VERSION:
        return "PRODUCTION_INSTALL_MEDIA_BAD_VERSION";
    case PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT:
        return "PRODUCTION_INSTALL_MEDIA_INVALID_ARGUMENT";
    case PRODUCTION_INSTALL_MEDIA_BAD_TEXT:
        return "PRODUCTION_INSTALL_MEDIA_BAD_TEXT";
    case PRODUCTION_INSTALL_MEDIA_OVERFLOW:
        return "PRODUCTION_INSTALL_MEDIA_OVERFLOW";
    default:
        return NULL;
    }
}

const char *production_install_media_controller_string(
    enum installer_disk_controller_kind controller)
{
    switch (controller) {
    case INSTALLER_DISK_CONTROLLER_VIRTIO_BLK:
        return "VIRTIO-BLK";
    case INSTALLER_DISK_CONTROLLER_SATA:
        return "SATA";
    case INSTALLER_DISK_CONTROLLER_NVME:
        return "NVME";
    case INSTALLER_DISK_CONTROLLER_UNKNOWN:
        return "UNKNOWN";
    default:
        return "UNKNOWN";
    }
}

const char *production_install_media_management_string(
    const struct production_install_media_declaration *declaration)
{
    if (declaration != NULL && declaration->management_interface) {
        return PRODUCTION_INSTALL_MEDIA_MANAGEMENT_INTERFACE;
    }
    return PRODUCTION_INSTALL_MEDIA_MANAGEMENT_LOCAL;
}
