#include "appliance_route_table.h"

/*
 * ITEM-504 part A: the facade's route table as a pure bounded value.
 *
 * Everything here is a function of bytes.  The name grammar, the backend
 * address rule, the duplicate rule and the fail-closed lookup are decided in
 * one place, so the installation declaration, the management panel and the
 * serving path cannot disagree about what a route means.  Nothing is ever
 * stored partially: every refusal happens before the first byte of the table
 * changes.
 */

struct appliance_route_writer {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
};

static size_t route_text_length(const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return 0U;
    }
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool route_is_digit(char character)
{
    return character >= '0' && character <= '9';
}

static bool route_is_lower_alnum(char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9');
}

static char route_to_lower(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return (char)(character - 'A' + 'a');
    }
    return character;
}

static bool route_text_equal(const char *left, const char *right, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

/*
 * A literal IPv4 address: four decimal parts of one to three digits, each
 * 0..255, no leading zero (so `010.0.0.1` is refused instead of being read as
 * two different addresses by two different readers).
 */
static bool route_address_parse(
    const char *text,
    size_t length,
    uint8_t out[APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH])
{
    size_t index;
    size_t digits = 0U;
    size_t part = 0U;
    uint32_t value = 0U;

    if (text == NULL || out == NULL || length == 0U || length > 15U) {
        return false;
    }
    for (index = 0U; index <= length; ++index) {
        if (index == length || text[index] == '.') {
            if (digits == 0U || part >= 4U) {
                return false;
            }
            if (digits > 1U && text[index - digits] == '0') {
                return false;
            }
            if (value > 255U) {
                return false;
            }
            out[part] = (uint8_t)value;
            ++part;
            digits = 0U;
            value = 0U;
            continue;
        }
        if (!route_is_digit(text[index]) || digits == 3U) {
            return false;
        }
        value = value * 10U + (uint32_t)(text[index] - '0');
        ++digits;
    }
    return part == 4U;
}

/*
 * A backend has to name one host: exactly as the ITEM-449 forward declaration,
 * the unspecified address, the limited broadcast, the loopback range and the
 * multicast range are not hosts a route can mean.
 */
static bool route_backend_is_host(
    const uint8_t address[APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH])
{
    if (address[0] == 0U && address[1] == 0U &&
        address[2] == 0U && address[3] == 0U) {
        return false;
    }
    if (address[0] == 255U && address[1] == 255U &&
        address[2] == 255U && address[3] == 255U) {
        return false;
    }
    if (address[0] == 127U) {
        return false;
    }
    if ((address[0] & 0xF0U) == 224U) {
        return false;
    }
    return true;
}

static bool route_port_parse(const char *text, size_t length, uint16_t *port_out)
{
    size_t index;
    uint32_t value = 0U;

    if (text == NULL || port_out == NULL || length == 0U || length > 5U) {
        return false;
    }
    if (length > 1U && text[0] == '0') {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (!route_is_digit(text[index])) {
            return false;
        }
        value = value * 10U + (uint32_t)(text[index] - '0');
    }
    if (value == 0U || value > 65535U) {
        return false;
    }
    *port_out = (uint16_t)value;
    return true;
}

enum appliance_route_status appliance_route_name_normalize(
    const char *name,
    size_t name_length,
    char *out,
    size_t capacity,
    size_t *length_out)
{
    size_t index;
    size_t written = 0U;

    if (name == NULL || out == NULL || length_out == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    if (name_length == 0U) {
        return APPLIANCE_ROUTE_BAD_NAME;
    }
    /* One trailing dot is the root label and is accepted, then removed. */
    if (name[name_length - 1U] == '.') {
        if (name_length == 1U) {
            return APPLIANCE_ROUTE_BAD_NAME;
        }
        --name_length;
    }
    if (name_length == 0U) {
        return APPLIANCE_ROUTE_BAD_NAME;
    }
    if (name_length > APPLIANCE_ROUTE_NAME_MAX) {
        return APPLIANCE_ROUTE_NAME_TOO_LONG;
    }
    if (capacity < name_length + 1U) {
        return APPLIANCE_ROUTE_BUFFER_TOO_SMALL;
    }
    for (index = 0U; index < name_length; ++index) {
        char character = name[index];

        if (character == '.') {
            if (index == 0U || index + 1U == name_length ||
                name[index - 1U] == '.') {
                return APPLIANCE_ROUTE_BAD_NAME;
            }
            out[written] = '.';
            ++written;
            continue;
        }
        character = route_to_lower(character);
        if (!route_is_lower_alnum(character) && character != '-') {
            return APPLIANCE_ROUTE_BAD_NAME;
        }
        out[written] = character;
        ++written;
    }
    /*
     * Every label is 1..63 bytes and never starts or ends with a hyphen: a
     * label like `-x` or a 64-byte label is not a name the facade could match
     * against a real Host header, so it is refused here rather than never
     * matching in the field.
     */
    {
        size_t start = 0U;

        for (index = 0U; index <= name_length; ++index) {
            if (index == name_length || name[index] == '.') {
                size_t label_length = index - start;

                if (label_length == 0U || label_length > 63U) {
                    return APPLIANCE_ROUTE_BAD_NAME;
                }
                if (name[start] == '-' || name[index - 1U] == '-') {
                    return APPLIANCE_ROUTE_BAD_NAME;
                }
                start = index + 1U;
            }
        }
    }
    out[written] = '\0';
    *length_out = written;
    return APPLIANCE_ROUTE_OK;
}

enum appliance_route_status appliance_route_name_from_host(
    const char *host,
    size_t host_length,
    char *out,
    size_t capacity,
    size_t *length_out)
{
    size_t index;
    size_t name_length = host_length;
    size_t colon = host_length;

    if (host == NULL || out == NULL || length_out == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    if (host_length == 0U) {
        return APPLIANCE_ROUTE_BAD_NAME;
    }
    for (index = 0U; index < host_length; ++index) {
        unsigned char character = (unsigned char)host[index];

        if (character <= 0x20U || character >= 0x7FU) {
            return APPLIANCE_ROUTE_BAD_NAME;
        }
        if (character == ':' && colon == host_length) {
            colon = index;
        }
    }
    /*
     * An IPv6 literal is a host the operator can connect to, but it is not a
     * public name this table routes: the facade matches the name the operator
     * declared, and refusing the literal here keeps that rule visible.
     */
    if (host[0] == '[') {
        return APPLIANCE_ROUTE_BAD_NAME;
    }
    if (colon != host_length) {
        uint16_t port = 0U;

        if (colon + 1U >= host_length) {
            return APPLIANCE_ROUTE_BAD_NAME;
        }
        for (index = colon + 1U; index < host_length; ++index) {
            if (host[index] == ':') {
                return APPLIANCE_ROUTE_BAD_NAME;
            }
        }
        if (!route_port_parse(host + colon + 1U, host_length - colon - 1U,
                              &port)) {
            return APPLIANCE_ROUTE_BAD_NAME;
        }
        name_length = colon;
    }
    return appliance_route_name_normalize(host, name_length, out, capacity,
                                          length_out);
}

static enum appliance_route_status route_validate_one(
    const struct appliance_route *route)
{
    char normalized[APPLIANCE_ROUTE_NAME_MAX + 1U];
    size_t length = 0U;
    enum appliance_route_status status;

    if (route == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    if (route->name_length == 0U ||
        route->name_length > APPLIANCE_ROUTE_NAME_MAX) {
        return APPLIANCE_ROUTE_BAD_NAME;
    }
    if (route->name[route->name_length] != '\0') {
        return APPLIANCE_ROUTE_BAD_NAME;
    }
    status = appliance_route_name_normalize(route->name, route->name_length,
                                            normalized, sizeof(normalized),
                                            &length);
    if (status != APPLIANCE_ROUTE_OK) {
        return status;
    }
    if (length != route->name_length ||
        !route_text_equal(route->name, normalized, length)) {
        return APPLIANCE_ROUTE_BAD_NAME;
    }
    if (!route_backend_is_host(route->backend)) {
        return APPLIANCE_ROUTE_BACKEND_NOT_HOST;
    }
    if (route->backend_port == 0U) {
        return APPLIANCE_ROUTE_BAD_PORT;
    }
    if (route->source != (uint32_t)APPLIANCE_ROUTE_SOURCE_DECLARATION &&
        route->source != (uint32_t)APPLIANCE_ROUTE_SOURCE_PANEL) {
        return APPLIANCE_ROUTE_BAD_SOURCE;
    }
    return APPLIANCE_ROUTE_OK;
}

void appliance_route_table_init(struct appliance_route_table *table)
{
    if (table == NULL) {
        return;
    }
    table->version = APPLIANCE_ROUTE_TABLE_VERSION;
    table->count = 0U;
}

enum appliance_route_status appliance_route_table_add(
    struct appliance_route_table *table,
    const char *name,
    size_t name_length,
    const char *backend,
    size_t backend_length,
    const char *port,
    size_t port_length,
    bool tls_to_backend,
    enum appliance_route_source source)
{
    struct appliance_route route = { 0 };
    enum appliance_route_status status;
    size_t normalized_length = 0U;
    uint32_t index;

    if (table == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    if (table->version != APPLIANCE_ROUTE_TABLE_VERSION) {
        return APPLIANCE_ROUTE_BAD_VERSION;
    }
    if (table->count >= APPLIANCE_ROUTE_TABLE_MAX) {
        return APPLIANCE_ROUTE_TABLE_FULL;
    }
    if (source != APPLIANCE_ROUTE_SOURCE_DECLARATION &&
        source != APPLIANCE_ROUTE_SOURCE_PANEL) {
        return APPLIANCE_ROUTE_BAD_SOURCE;
    }
    status = appliance_route_name_normalize(name, name_length, route.name,
                                            sizeof(route.name),
                                            &normalized_length);
    if (status != APPLIANCE_ROUTE_OK) {
        return status;
    }
    route.name_length = (uint32_t)normalized_length;
    if (!route_address_parse(backend, backend_length, route.backend)) {
        return APPLIANCE_ROUTE_BAD_BACKEND;
    }
    if (!route_backend_is_host(route.backend)) {
        return APPLIANCE_ROUTE_BACKEND_NOT_HOST;
    }
    if (!route_port_parse(port, port_length, &route.backend_port)) {
        return APPLIANCE_ROUTE_BAD_PORT;
    }
    route.tls_to_backend = tls_to_backend;
    route.source = (uint32_t)source;
    for (index = 0U; index < table->count; ++index) {
        if (table->routes[index].name_length == route.name_length &&
            route_text_equal(table->routes[index].name, route.name,
                             route.name_length)) {
            return APPLIANCE_ROUTE_DUPLICATE_NAME;
        }
    }
    table->routes[table->count] = route;
    ++table->count;
    return APPLIANCE_ROUTE_OK;
}

enum appliance_route_status appliance_route_table_remove(
    struct appliance_route_table *table,
    const char *name,
    size_t name_length)
{
    char normalized[APPLIANCE_ROUTE_NAME_MAX + 1U];
    enum appliance_route_status status;
    size_t length = 0U;
    uint32_t index;

    if (table == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    if (table->version != APPLIANCE_ROUTE_TABLE_VERSION) {
        return APPLIANCE_ROUTE_BAD_VERSION;
    }
    status = appliance_route_name_normalize(name, name_length, normalized,
                                            sizeof(normalized), &length);
    if (status != APPLIANCE_ROUTE_OK) {
        return status;
    }
    for (index = 0U; index < table->count; ++index) {
        if (table->routes[index].name_length == (uint32_t)length &&
            route_text_equal(table->routes[index].name, normalized, length)) {
            uint32_t move;

            for (move = index; move + 1U < table->count; ++move) {
                table->routes[move] = table->routes[move + 1U];
            }
            --table->count;
            return APPLIANCE_ROUTE_OK;
        }
    }
    return APPLIANCE_ROUTE_NOT_FOUND;
}

enum appliance_route_status appliance_route_table_lookup(
    const struct appliance_route_table *table,
    const char *name,
    size_t name_length,
    struct appliance_route *route_out)
{
    char normalized[APPLIANCE_ROUTE_NAME_MAX + 1U];
    enum appliance_route_status status;
    size_t length = 0U;
    uint32_t index;

    if (table == NULL || route_out == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    if (table->version != APPLIANCE_ROUTE_TABLE_VERSION) {
        return APPLIANCE_ROUTE_BAD_VERSION;
    }
    if (table->count > APPLIANCE_ROUTE_TABLE_MAX) {
        return APPLIANCE_ROUTE_BAD_VERSION;
    }
    status = appliance_route_name_normalize(name, name_length, normalized,
                                            sizeof(normalized), &length);
    if (status != APPLIANCE_ROUTE_OK) {
        return status;
    }
    for (index = 0U; index < table->count; ++index) {
        if (table->routes[index].name_length == (uint32_t)length &&
            route_text_equal(table->routes[index].name, normalized, length)) {
            *route_out = table->routes[index];
            return APPLIANCE_ROUTE_OK;
        }
    }
    return APPLIANCE_ROUTE_NOT_FOUND;
}

/* One empty route: no name, no backend, no port, no source, no stale bytes. */
static void route_clear(struct appliance_route *route)
{
    unsigned char *bytes = (unsigned char *)route;
    size_t index;

    for (index = 0U; index < sizeof(*route); ++index) {
        bytes[index] = 0U;
    }
}

enum appliance_route_status appliance_route_table_resolve_host(
    const struct appliance_route_table *table,
    const char *host,
    size_t host_length,
    struct appliance_route *route_out)
{
    char name[APPLIANCE_ROUTE_NAME_MAX + 1U];
    size_t name_length = 0U;
    enum appliance_route_status status;

    if (table == NULL || host == NULL || route_out == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    /*
     * Fail closed before anything else: a refusal must not leave the caller
     * holding any byte of a previous request's route.  The whole value is
     * cleared, not only the fields a reader happens to look at first, so a
     * caller that copies the struct cannot carry a stale name out of a
     * refused request.
     */
    route_clear(route_out);
    status = appliance_route_name_from_host(host, host_length, name,
                                            sizeof(name), &name_length);
    if (status != APPLIANCE_ROUTE_OK) {
        return status;
    }
    return appliance_route_table_lookup(table, name, name_length, route_out);
}

enum appliance_route_status appliance_route_table_validate(
    const struct appliance_route_table *table)
{
    uint32_t index;

    if (table == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    if (table->version != APPLIANCE_ROUTE_TABLE_VERSION) {
        return APPLIANCE_ROUTE_BAD_VERSION;
    }
    if (table->count > APPLIANCE_ROUTE_TABLE_MAX) {
        return APPLIANCE_ROUTE_BAD_VERSION;
    }
    for (index = 0U; index < table->count; ++index) {
        enum appliance_route_status status =
            route_validate_one(&table->routes[index]);
        uint32_t other;

        if (status != APPLIANCE_ROUTE_OK) {
            return status;
        }
        for (other = 0U; other < index; ++other) {
            if (table->routes[other].name_length ==
                    table->routes[index].name_length &&
                route_text_equal(table->routes[other].name,
                                 table->routes[index].name,
                                 table->routes[index].name_length)) {
                return APPLIANCE_ROUTE_DUPLICATE_NAME;
            }
        }
    }
    return APPLIANCE_ROUTE_OK;
}

static void route_writer_init(
    struct appliance_route_writer *writer,
    char *buffer,
    size_t capacity)
{
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->length = 0U;
    writer->overflow = false;
}

static void route_writer_text(
    struct appliance_route_writer *writer,
    const char *text,
    size_t length)
{
    size_t index;

    if (writer->overflow || text == NULL) {
        return;
    }
    if (writer->length + length + 1U > writer->capacity) {
        writer->overflow = true;
        return;
    }
    for (index = 0U; index < length; ++index) {
        writer->buffer[writer->length + index] = text[index];
    }
    writer->length += length;
}

static void route_writer_string(
    struct appliance_route_writer *writer,
    const char *text)
{
    route_writer_text(writer, text, route_text_length(text));
}

static void route_writer_u32(struct appliance_route_writer *writer, uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    if (value == 0U) {
        route_writer_text(writer, "0", 1U);
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        --count;
        route_writer_text(writer, &digits[count], 1U);
    }
}

static void route_writer_address(
    struct appliance_route_writer *writer,
    const uint8_t address[APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH])
{
    size_t index;

    for (index = 0U; index < APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH; ++index) {
        if (index != 0U) {
            route_writer_text(writer, ".", 1U);
        }
        route_writer_u32(writer, (uint32_t)address[index]);
    }
}

static enum appliance_route_status route_render_body(
    const struct appliance_route *route,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    struct appliance_route_writer writer;
    enum appliance_route_status status = route_validate_one(route);

    if (status != APPLIANCE_ROUTE_OK) {
        return status;
    }
    if (buffer == NULL || length_out == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    route_writer_init(&writer, buffer, capacity);
    route_writer_string(&writer, "name=");
    route_writer_text(&writer, route->name, route->name_length);
    route_writer_string(&writer, " backend=");
    route_writer_address(&writer, route->backend);
    route_writer_text(&writer, ":", 1U);
    route_writer_u32(&writer, (uint32_t)route->backend_port);
    route_writer_string(&writer, " tls=");
    route_writer_string(&writer,
                        route->tls_to_backend ? "on" : "off");
    route_writer_string(&writer, " source=");
    route_writer_string(&writer,
                        appliance_route_source_string(
                            (enum appliance_route_source)route->source));
    if (writer.overflow || writer.length + 1U > writer.capacity) {
        return APPLIANCE_ROUTE_BUFFER_TOO_SMALL;
    }
    writer.buffer[writer.length] = '\0';
    *length_out = writer.length;
    return APPLIANCE_ROUTE_OK;
}

enum appliance_route_status appliance_route_render(
    const struct appliance_route *route,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    return route_render_body(route, buffer, capacity, length_out);
}

enum appliance_route_status appliance_route_table_render(
    const struct appliance_route_table *table,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    struct appliance_route_writer writer;
    enum appliance_route_status status;
    uint32_t index;

    if (table == NULL || buffer == NULL || length_out == NULL) {
        return APPLIANCE_ROUTE_NULL;
    }
    status = appliance_route_table_validate(table);
    if (status != APPLIANCE_ROUTE_OK) {
        return status;
    }
    route_writer_init(&writer, buffer, capacity);
    route_writer_string(&writer, "routes=");
    route_writer_u32(&writer, table->count);
    for (index = 0U; index < table->count; ++index) {
        char body[APPLIANCE_ROUTE_RECORD_MAX + 1U];
        size_t body_length = 0U;

        status = route_render_body(&table->routes[index], body, sizeof(body),
                                   &body_length);
        if (status != APPLIANCE_ROUTE_OK) {
            return status;
        }
        route_writer_string(&writer, "; ");
        route_writer_text(&writer, body, body_length);
    }
    if (writer.overflow || writer.length + 1U > writer.capacity) {
        return APPLIANCE_ROUTE_BUFFER_TOO_SMALL;
    }
    writer.buffer[writer.length] = '\0';
    *length_out = writer.length;
    return APPLIANCE_ROUTE_OK;
}

const char *appliance_route_source_string(enum appliance_route_source source)
{
    switch (source) {
    case APPLIANCE_ROUTE_SOURCE_NONE:
        return "NONE";
    case APPLIANCE_ROUTE_SOURCE_DECLARATION:
        return "DECLARATION";
    case APPLIANCE_ROUTE_SOURCE_PANEL:
        return "PANEL";
    }
    return "UNKNOWN";
}

const char *appliance_route_status_string(enum appliance_route_status status)
{
    switch (status) {
    case APPLIANCE_ROUTE_OK:
        return "OK";
    case APPLIANCE_ROUTE_NULL:
        return "NULL";
    case APPLIANCE_ROUTE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_ROUTE_BAD_NAME:
        return "BAD_NAME";
    case APPLIANCE_ROUTE_NAME_TOO_LONG:
        return "NAME_TOO_LONG";
    case APPLIANCE_ROUTE_BAD_BACKEND:
        return "BAD_BACKEND";
    case APPLIANCE_ROUTE_BACKEND_NOT_HOST:
        return "BACKEND_NOT_HOST";
    case APPLIANCE_ROUTE_BAD_PORT:
        return "BAD_PORT";
    case APPLIANCE_ROUTE_BAD_SOURCE:
        return "BAD_SOURCE";
    case APPLIANCE_ROUTE_DUPLICATE_NAME:
        return "DUPLICATE_NAME";
    case APPLIANCE_ROUTE_TABLE_FULL:
        return "TABLE_FULL";
    case APPLIANCE_ROUTE_NOT_FOUND:
        return "NOT_FOUND";
    case APPLIANCE_ROUTE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    }
    return "UNKNOWN";
}
