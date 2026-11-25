#ifndef HFOC_APPLIANCE_ROUTE_TABLE_H
#define HFOC_APPLIANCE_ROUTE_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-504 part A: the facade's route table.
 *
 * The appliance is an internet facade: it terminates TLS and must lead a
 * connection to the backend the operator declared for the public name in the
 * request.  This module owns that declaration as a pure bounded value - the
 * name, the literal IPv4 backend, the backend port, whether the hop to the
 * backend is itself TLS, and where the route came from (the installation
 * declaration or the management panel).
 *
 * It owns no socket, no listener and no state store: the facade, the panel and
 * (later) the persisted configuration all produce and consume this one value.
 * The rules are deliberately strict because the table is an access decision:
 *
 *   * a name is a DNS name, normalized to lower case, at most 253 bytes,
 *     labels of 1..63 bytes from `[a-z0-9-]` that never start or end with a
 *     hyphen; a trailing dot is accepted and removed, a wildcard is not a
 *     name this table can mean;
 *   * a backend is a literal IPv4 address that names one host: the
 *     unspecified address, the limited broadcast, loopback and multicast are
 *     refused, exactly as the ITEM-449 forward declaration refuses them;
 *   * the backend port is 1..65535;
 *   * two routes never carry the same name, and the table holds at most
 *     APPLIANCE_ROUTE_TABLE_MAX routes;
 *   * a lookup is fail-closed: an unknown name is refused with the reason
 *     that names it, never resolved to the first route that happens to exist.
 */

#define APPLIANCE_ROUTE_TABLE_VERSION 1U
#define APPLIANCE_ROUTE_TABLE_MAX 8U
#define APPLIANCE_ROUTE_NAME_MAX 253U
#define APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH 4U
/*
 * One `name=... backend=... tls=... source=...` record body: the 253-byte
 * name maximum, the 15-byte literal address, a five-digit port and the
 * longest source token, plus the fixed words and the terminator.
 */
#define APPLIANCE_ROUTE_RECORD_MAX 320U
/* A suggested capacity for the whole-table rendering (never a truncation). */
#define APPLIANCE_ROUTE_LIST_MAX 2688U

enum appliance_route_source {
    APPLIANCE_ROUTE_SOURCE_NONE = 0,
    APPLIANCE_ROUTE_SOURCE_DECLARATION = 1,
    APPLIANCE_ROUTE_SOURCE_PANEL = 2,
};

enum appliance_route_status {
    APPLIANCE_ROUTE_OK = 0,
    APPLIANCE_ROUTE_NULL = 1,
    APPLIANCE_ROUTE_BAD_VERSION = 2,
    APPLIANCE_ROUTE_BAD_NAME = 3,
    APPLIANCE_ROUTE_NAME_TOO_LONG = 4,
    APPLIANCE_ROUTE_BAD_BACKEND = 5,
    APPLIANCE_ROUTE_BACKEND_NOT_HOST = 6,
    APPLIANCE_ROUTE_BAD_PORT = 7,
    APPLIANCE_ROUTE_BAD_SOURCE = 8,
    APPLIANCE_ROUTE_DUPLICATE_NAME = 9,
    APPLIANCE_ROUTE_TABLE_FULL = 10,
    APPLIANCE_ROUTE_NOT_FOUND = 11,
    APPLIANCE_ROUTE_BUFFER_TOO_SMALL = 12,
};

struct appliance_route {
    /* Normalized (lower case, no trailing dot) and NUL-terminated. */
    char name[APPLIANCE_ROUTE_NAME_MAX + 1U];
    uint32_t name_length;
    uint8_t backend[APPLIANCE_ROUTE_BACKEND_ADDRESS_LENGTH];
    uint16_t backend_port;
    bool tls_to_backend;
    uint32_t source;
};

struct appliance_route_table {
    uint32_t version;
    uint32_t count;
    struct appliance_route routes[APPLIANCE_ROUTE_TABLE_MAX];
};

/* An empty table is a valid table: it routes nothing and refuses every name. */
void appliance_route_table_init(struct appliance_route_table *table);

/*
 * Add one route.  The name is normalized and validated, the backend text is
 * parsed as a literal host IPv4 address, the port is parsed as decimal 1..65535,
 * and the source has to be one of the two producers.  A duplicate name (in any
 * case) and a full table are refused; nothing is stored on any refusal.
 */
enum appliance_route_status appliance_route_table_add(
    struct appliance_route_table *table,
    const char *name,
    size_t name_length,
    const char *backend,
    size_t backend_length,
    const char *port,
    size_t port_length,
    bool tls_to_backend,
    enum appliance_route_source source);

/* Remove one route by name; a name the table does not carry is NOT_FOUND. */
enum appliance_route_status appliance_route_table_remove(
    struct appliance_route_table *table,
    const char *name,
    size_t name_length);

/*
 * The access decision.  `name` is a bare name, not a Host header: a malformed
 * name is BAD_NAME and a well-formed name the table does not carry is
 * NOT_FOUND, so the caller can publish why the connection was refused.  The
 * matched route is copied into `route_out` and never aliases the table.
 */
enum appliance_route_status appliance_route_table_lookup(
    const struct appliance_route_table *table,
    const char *name,
    size_t name_length,
    struct appliance_route *route_out);

/* Check a table that may have been built by hand or read from storage. */
enum appliance_route_status appliance_route_table_validate(
    const struct appliance_route_table *table);

/*
 * Render one route as the record body
 * `name=<name> backend=<a.b.c.d>:<port> tls=<on|off> source=<source>`.
 * Rendering never truncates: a buffer that cannot hold the whole body is
 * refused with BUFFER_TOO_SMALL.
 */
enum appliance_route_status appliance_route_render(
    const struct appliance_route *route,
    char *buffer,
    size_t capacity,
    size_t *length_out);
/* Render the whole table as `routes=<n>` followed by one `;`-separated body
 * per route, in table order. */
enum appliance_route_status appliance_route_table_render(
    const struct appliance_route_table *table,
    char *buffer,
    size_t capacity,
    size_t *length_out);

/*
 * Normalize and validate a bare name: lower case, one trailing dot accepted
 * and removed.  `out` receives a NUL-terminated copy.
 */
enum appliance_route_status appliance_route_name_normalize(
    const char *name,
    size_t name_length,
    char *out,
    size_t capacity,
    size_t *length_out);

/*
 * Extract the routing name from an HTTP `Host` header value: an optional
 * `:port` is accepted and removed, an IPv6 literal (`[..]`) is refused as a
 * name this table cannot carry, and anything with a control byte, a space or
 * a non-ASCII byte is refused.  The name is normalized as above.
 */
enum appliance_route_status appliance_route_name_from_host(
    const char *host,
    size_t host_length,
    char *out,
    size_t capacity,
    size_t *length_out);

/*
 * ITEM-504 increment 7: the whole decision the facade makes for one request -
 * the `Host` header value a client sent, resolved to the route the operator
 * declared for that name.  The name keeps its own reason (a `Host` this table
 * cannot carry, including an absent or empty one, is BAD_NAME) and the lookup
 * keeps its own (a well-formed name the table does not carry is NOT_FOUND), so
 * the caller publishes why the connection was refused instead of leading it to
 * another backend.  `route_out` never aliases the table and is zeroed on every
 * refusal, so a refused request cannot be served from a stale route.
 */
enum appliance_route_status appliance_route_table_resolve_host(
    const struct appliance_route_table *table,
    const char *host,
    size_t host_length,
    struct appliance_route *route_out);

const char *appliance_route_source_string(enum appliance_route_source source);
const char *appliance_route_status_string(enum appliance_route_status status);

#endif
