/*
 * ITEM-504 host proof for the facade route table.
 *
 * The route table is an access decision: which public name leads to which
 * backend, and which names lead nowhere.  This proof is deliberately
 * exhaustive on the refusals, because "the operator's name reaches a backend
 * or is refused with a reason" is only as good as these decisions.
 */
#include "appliance_route_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static enum appliance_route_status add(
    struct appliance_route_table *table,
    const char *name,
    const char *backend,
    const char *port,
    bool tls,
    enum appliance_route_source source)
{
    return appliance_route_table_add(
        table,
        name, name != NULL ? strlen(name) : 0U,
        backend, backend != NULL ? strlen(backend) : 0U,
        port, port != NULL ? strlen(port) : 0U,
        tls, source);
}

static bool name_is(const struct appliance_route *route, const char *expected)
{
    size_t length = strlen(expected);

    return route->name_length == (uint32_t)length &&
           memcmp(route->name, expected, length) == 0;
}

/* A refused resolution must leave no byte of a previous route behind. */
static int route_is_empty(const struct appliance_route *route)
{
    const unsigned char *bytes = (const unsigned char *)route;
    size_t index;

    for (index = 0U; index < sizeof(*route); ++index) {
        if (bytes[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static void test_empty_table(void)
{
    struct appliance_route_table table;
    struct appliance_route route;
    char rendered[APPLIANCE_ROUTE_LIST_MAX];
    size_t length = 0U;

    appliance_route_table_init(&table);
    CHECK(table.version == APPLIANCE_ROUTE_TABLE_VERSION,
          "a new table carries the current version");
    CHECK(table.count == 0U, "a new table carries no routes");
    CHECK(appliance_route_table_validate(&table) == APPLIANCE_ROUTE_OK,
          "an empty table is a valid table");
    CHECK(appliance_route_table_lookup(&table, "example.com",
                                       strlen("example.com"),
                                       &route) == APPLIANCE_ROUTE_NOT_FOUND,
          "an empty table refuses every name as not found");
    CHECK(appliance_route_table_render(&table, rendered, sizeof(rendered),
                                       &length) == APPLIANCE_ROUTE_OK,
          "an empty table renders");
    CHECK(strcmp(rendered, "routes=0") == 0,
          "an empty table renders exactly routes=0");
    CHECK(length == strlen("routes=0"), "the rendered length is exact");
}

static void test_add_and_lookup(void)
{
    struct appliance_route_table table;
    struct appliance_route route;

    appliance_route_table_init(&table);
    CHECK(add(&table, "example.com", "10.0.0.5", "8443", true,
              APPLIANCE_ROUTE_SOURCE_PANEL) == APPLIANCE_ROUTE_OK,
          "a well-formed panel route is added");
    CHECK(table.count == 1U, "the route count grows by one");
    CHECK(appliance_route_table_lookup(&table, "example.com",
                                       strlen("example.com"),
                                       &route) == APPLIANCE_ROUTE_OK,
          "the declared name is found");
    CHECK(name_is(&route, "example.com"), "the found route names the name");
    CHECK(route.backend[0] == 10U && route.backend[1] == 0U &&
          route.backend[2] == 0U && route.backend[3] == 5U,
          "the found route carries the backend");
    CHECK(route.backend_port == 8443U, "the found route carries the port");
    CHECK(route.tls_to_backend, "the found route carries the TLS flag");
    CHECK(route.source == (uint32_t)APPLIANCE_ROUTE_SOURCE_PANEL,
          "the found route carries its source");
    CHECK(appliance_route_table_lookup(&table, "EXAMPLE.COM",
                                       strlen("EXAMPLE.COM"),
                                       &route) == APPLIANCE_ROUTE_OK,
          "a lookup is case-insensitive");
    CHECK(name_is(&route, "example.com"),
          "a case-insensitive lookup returns the normalized name");
    CHECK(appliance_route_table_lookup(&table, "example.com.",
                                       strlen("example.com."),
                                       &route) == APPLIANCE_ROUTE_OK,
          "a trailing root dot is accepted");
    CHECK(appliance_route_table_lookup(&table, "example.com:443",
                                       strlen("example.com:443"),
                                       &route) == APPLIANCE_ROUTE_BAD_NAME,
          "a bare lookup does not accept a Host port suffix");
    CHECK(appliance_route_table_lookup(&table, "other.example",
                                       strlen("other.example"),
                                       &route) == APPLIANCE_ROUTE_NOT_FOUND,
          "an unknown name is refused as not found, never matched to the first route");
    CHECK(add(&table, "declared.example", "10.0.0.6", "80", false,
              APPLIANCE_ROUTE_SOURCE_DECLARATION) == APPLIANCE_ROUTE_OK,
          "a declaration route is added beside it");
    CHECK(table.count == 2U, "both producers share one table");
}

static void test_duplicate_names(void)
{
    struct appliance_route_table table;

    appliance_route_table_init(&table);
    CHECK(add(&table, "example.com", "10.0.0.5", "8443", true,
              APPLIANCE_ROUTE_SOURCE_PANEL) == APPLIANCE_ROUTE_OK,
          "the first name is added");
    CHECK(add(&table, "EXAMPLE.COM", "10.0.0.9", "443", false,
              APPLIANCE_ROUTE_SOURCE_DECLARATION) ==
          APPLIANCE_ROUTE_DUPLICATE_NAME,
          "a duplicate name in another case is refused");
    CHECK(table.count == 1U, "a refused duplicate does not change the table");
    CHECK(add(&table, "example.com.", "10.0.0.9", "443", false,
              APPLIANCE_ROUTE_SOURCE_DECLARATION) ==
          APPLIANCE_ROUTE_DUPLICATE_NAME,
          "a duplicate name with a trailing dot is refused");
}

static void test_invalid_names(void)
{
    struct appliance_route_table table;
    struct {
        const char *name;
        enum appliance_route_status status;
    } cases[] = {
        { "", APPLIANCE_ROUTE_BAD_NAME },
        { ".", APPLIANCE_ROUTE_BAD_NAME },
        { ".example.com", APPLIANCE_ROUTE_BAD_NAME },
        { "example..com", APPLIANCE_ROUTE_BAD_NAME },
        { "example.com..", APPLIANCE_ROUTE_BAD_NAME },
        { "-example.com", APPLIANCE_ROUTE_BAD_NAME },
        { "example-.com", APPLIANCE_ROUTE_BAD_NAME },
        { "exa_mple.com", APPLIANCE_ROUTE_BAD_NAME },
        { "*.example.com", APPLIANCE_ROUTE_BAD_NAME },
        { "example .com", APPLIANCE_ROUTE_BAD_NAME },
        { "example.com:443", APPLIANCE_ROUTE_BAD_NAME },
        { "example/../com", APPLIANCE_ROUTE_BAD_NAME },
        { "\xd0\xbf\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd1\x80.\xd1\x80\xd1\x84",
          APPLIANCE_ROUTE_BAD_NAME },
    };
    size_t index;

    appliance_route_table_init(&table);
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char message[128];

        snprintf(message, sizeof(message), "the name `%s` is refused",
                 cases[index].name);
        CHECK(add(&table, cases[index].name, "10.0.0.5", "8443", false,
                  APPLIANCE_ROUTE_SOURCE_PANEL) == cases[index].status,
              message);
    }
    {
        char label[65];
        char name[300];

        memset(label, 'a', 64U);
        label[64] = '\0';
        snprintf(name, sizeof(name), "%s.example", label);
        CHECK(add(&table, name, "10.0.0.5", "8443", false,
                  APPLIANCE_ROUTE_SOURCE_PANEL) == APPLIANCE_ROUTE_BAD_NAME,
              "a 64-byte label is refused");
    }
    {
        char long_name[300];
        size_t written = 0U;
        size_t part;

        for (part = 0U; part < 4U; ++part) {
            size_t index2;

            if (part != 0U) {
                long_name[written] = '.';
                ++written;
            }
            for (index2 = 0U; index2 < 64U; ++index2) {
                long_name[written] = 'b';
                ++written;
            }
        }
        long_name[written] = '\0';
        CHECK(add(&table, long_name, "10.0.0.5", "8443", false,
                  APPLIANCE_ROUTE_SOURCE_PANEL) ==
              APPLIANCE_ROUTE_NAME_TOO_LONG,
              "a name over 253 bytes is refused as too long");
    }
    CHECK(table.count == 0U, "no refused name ever enters the table");
}

static void test_host_header_names(void)
{
    struct {
        const char *host;
        const char *expected;
        enum appliance_route_status status;
    } cases[] = {
        { "example.com", "example.com", APPLIANCE_ROUTE_OK },
        { "Example.COM.", "example.com", APPLIANCE_ROUTE_OK },
        { "example.com:8443", "example.com", APPLIANCE_ROUTE_OK },
        { "example.com:443", "example.com", APPLIANCE_ROUTE_OK },
        { "api.example.com:65535", "api.example.com", APPLIANCE_ROUTE_OK },
        { "", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "example.com:", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "example.com:0", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "example.com:65536", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "example.com:8a", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "example.com:443:1", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "[::1]:443", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "[2001:db8::1]", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "exa mple.com", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "example\t.com", NULL, APPLIANCE_ROUTE_BAD_NAME },
        { "\x7f.example.com", NULL, APPLIANCE_ROUTE_BAD_NAME },
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char normalized[APPLIANCE_ROUTE_NAME_MAX + 1U];
        size_t length = 0U;
        enum appliance_route_status status = appliance_route_name_from_host(
            cases[index].host, strlen(cases[index].host), normalized,
            sizeof(normalized), &length);
        char message[128];

        snprintf(message, sizeof(message),
                 "the Host value `%s` resolves as declared", cases[index].host);
        CHECK(status == cases[index].status, message);
        if (status == APPLIANCE_ROUTE_OK) {
            CHECK(length == strlen(cases[index].expected) &&
                  strcmp(normalized, cases[index].expected) == 0,
                  "the Host value normalizes to the declared name");
        }
    }
}

/*
 * ITEM-504 increment 7: the facade's whole decision for one request.  The
 * `Host` header value the client sent must resolve to the declared route of
 * that name - the port a browser adds is not part of the name, the name is
 * matched case-insensitively, an unknown name is refused as NOT_FOUND with the
 * reason that names it, and a refusal never leaves a route behind.
 */
static void test_resolve_host(void)
{
    struct appliance_route_table table;
    struct appliance_route route;
    struct {
        const char *host;
        enum appliance_route_status status;
    } cases[] = {
        { "example.com", APPLIANCE_ROUTE_OK },
        { "example.com:443", APPLIANCE_ROUTE_OK },
        { "EXAMPLE.COM", APPLIANCE_ROUTE_OK },
        { "example.com.", APPLIANCE_ROUTE_OK },
        { "other.example", APPLIANCE_ROUTE_NOT_FOUND },
        { "example.com.evil", APPLIANCE_ROUTE_NOT_FOUND },
        { "", APPLIANCE_ROUTE_BAD_NAME },
        { "example.com:", APPLIANCE_ROUTE_BAD_NAME },
        { "exa mple.com", APPLIANCE_ROUTE_BAD_NAME },
        { "[::1]:443", APPLIANCE_ROUTE_BAD_NAME },
    };
    size_t index;

    appliance_route_table_init(&table);
    CHECK(add(&table, "example.com", "10.0.0.5", "8443", true,
              APPLIANCE_ROUTE_SOURCE_PANEL) == APPLIANCE_ROUTE_OK,
          "the resolve fixture route is added");
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char message[160];

        memset(&route, 0xAA, sizeof(route));
        snprintf(message, sizeof(message),
                 "the Host value `%s` resolves as declared", cases[index].host);
        CHECK(appliance_route_table_resolve_host(
                  &table, cases[index].host, strlen(cases[index].host),
                  &route) == cases[index].status,
              message);
        if (cases[index].status == APPLIANCE_ROUTE_OK) {
            CHECK(name_is(&route, "example.com"),
                  "the resolved route is the declared route of that name");
            CHECK(route.backend[0] == 10U && route.backend[1] == 0U &&
                  route.backend[2] == 0U && route.backend[3] == 5U,
                  "the resolved route carries the declared backend");
            CHECK(route.backend_port == 8443U,
                  "the resolved route carries the declared port");
            CHECK(route.tls_to_backend,
                  "the resolved route carries the declared TLS flag");
        } else {
            CHECK(route_is_empty(&route),
                  "a refused Host leaves no route behind, byte for byte");
        }
    }
    CHECK(appliance_route_table_resolve_host(NULL, "example.com",
                                             strlen("example.com"),
                                             &route) == APPLIANCE_ROUTE_NULL,
          "a resolution without a table is refused");
    CHECK(appliance_route_table_resolve_host(&table, NULL, 0U, &route) ==
          APPLIANCE_ROUTE_NULL,
          "a resolution without a Host value is refused");
    CHECK(appliance_route_table_resolve_host(&table, "example.com",
                                             strlen("example.com"),
                                             NULL) == APPLIANCE_ROUTE_NULL,
          "a resolution without an output route is refused");
}

static void test_invalid_backends(void)
{
    struct appliance_route_table table;
    struct {
        const char *backend;
        enum appliance_route_status status;
    } cases[] = {
        { "10.0.0", APPLIANCE_ROUTE_BAD_BACKEND },
        { "10.0.0.1.5", APPLIANCE_ROUTE_BAD_BACKEND },
        { "10.0.0.", APPLIANCE_ROUTE_BAD_BACKEND },
        { "10.0.0.256", APPLIANCE_ROUTE_BAD_BACKEND },
        { "010.0.0.1", APPLIANCE_ROUTE_BAD_BACKEND },
        { "10.0.0.00", APPLIANCE_ROUTE_BAD_BACKEND },
        { "a.b.c.d", APPLIANCE_ROUTE_BAD_BACKEND },
        { "10.0.0.1x", APPLIANCE_ROUTE_BAD_BACKEND },
        { "", APPLIANCE_ROUTE_BAD_BACKEND },
        { "0.0.0.0", APPLIANCE_ROUTE_BACKEND_NOT_HOST },
        { "127.0.0.1", APPLIANCE_ROUTE_BACKEND_NOT_HOST },
        { "127.1.2.3", APPLIANCE_ROUTE_BACKEND_NOT_HOST },
        { "255.255.255.255", APPLIANCE_ROUTE_BACKEND_NOT_HOST },
        { "224.0.0.1", APPLIANCE_ROUTE_BACKEND_NOT_HOST },
        { "239.255.255.250", APPLIANCE_ROUTE_BACKEND_NOT_HOST },
        { "10.0.0.1", APPLIANCE_ROUTE_OK },
        { "192.168.1.254", APPLIANCE_ROUTE_OK },
        { "192.0.2.10", APPLIANCE_ROUTE_OK },
    };
    size_t index;

    appliance_route_table_init(&table);
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char message[128];
        char name[32];

        snprintf(name, sizeof(name), "backend%u.example",
                 (unsigned int)index);
        snprintf(message, sizeof(message),
                 "the backend %s is decided as declared", cases[index].backend);
        CHECK(add(&table, name, cases[index].backend, "8443", false,
                  APPLIANCE_ROUTE_SOURCE_PANEL) == cases[index].status,
              message);
    }
}

static void test_invalid_ports(void)
{
    struct appliance_route_table table;
    struct {
        const char *port;
        enum appliance_route_status status;
    } cases[] = {
        { "", APPLIANCE_ROUTE_BAD_PORT },
        { "0", APPLIANCE_ROUTE_BAD_PORT },
        { "65536", APPLIANCE_ROUTE_BAD_PORT },
        { "8443x", APPLIANCE_ROUTE_BAD_PORT },
        { "08", APPLIANCE_ROUTE_BAD_PORT },
        { " 443", APPLIANCE_ROUTE_BAD_PORT },
        { "1", APPLIANCE_ROUTE_OK },
        { "443", APPLIANCE_ROUTE_OK },
        { "65535", APPLIANCE_ROUTE_OK },
    };
    size_t index;

    appliance_route_table_init(&table);
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char message[96];
        char name[32];

        snprintf(name, sizeof(name), "port%u.example", (unsigned int)index);
        snprintf(message, sizeof(message), "the port `%s` is decided as declared",
                 cases[index].port);
        CHECK(add(&table, name, "10.0.0.5", cases[index].port, false,
                  APPLIANCE_ROUTE_SOURCE_PANEL) == cases[index].status,
              message);
    }
}

static void test_bad_source(void)
{
    struct appliance_route_table table;

    appliance_route_table_init(&table);
    CHECK(add(&table, "example.com", "10.0.0.5", "8443", false,
              APPLIANCE_ROUTE_SOURCE_NONE) == APPLIANCE_ROUTE_BAD_SOURCE,
          "a route without a producer is refused");
    CHECK(add(&table, "example.com", "10.0.0.5", "8443", false,
              (enum appliance_route_source)99) == APPLIANCE_ROUTE_BAD_SOURCE,
          "an unknown producer is refused");
}

static void test_table_full_and_remove(void)
{
    struct appliance_route_table table;
    struct appliance_route route;
    char name[32];
    uint32_t index;

    appliance_route_table_init(&table);
    for (index = 0U; index < APPLIANCE_ROUTE_TABLE_MAX; ++index) {
        snprintf(name, sizeof(name), "host%u.example", (unsigned int)index);
        CHECK(add(&table, name, "10.0.0.5", "8443", false,
                  APPLIANCE_ROUTE_SOURCE_PANEL) == APPLIANCE_ROUTE_OK,
              "a route fits while the table has room");
    }
    CHECK(table.count == APPLIANCE_ROUTE_TABLE_MAX,
          "the table holds the full route count");
    CHECK(add(&table, "overflow.example", "10.0.0.5", "8443", false,
              APPLIANCE_ROUTE_SOURCE_PANEL) == APPLIANCE_ROUTE_TABLE_FULL,
          "one route over the limit is refused");
    CHECK(appliance_route_table_remove(&table, "host3.example",
                                       strlen("host3.example")) ==
          APPLIANCE_ROUTE_OK,
          "an existing route is removed");
    CHECK(appliance_route_table_lookup(&table, "host3.example",
                                       strlen("host3.example"),
                                       &route) == APPLIANCE_ROUTE_NOT_FOUND,
          "the removed route is gone");
    CHECK(appliance_route_table_remove(&table, "host3.example",
                                       strlen("host3.example")) ==
          APPLIANCE_ROUTE_NOT_FOUND,
          "removing a name the table does not carry is refused");
    CHECK(appliance_route_table_remove(&table, "bad..name",
                                       strlen("bad..name")) ==
          APPLIANCE_ROUTE_BAD_NAME,
          "removing a malformed name is refused by name");
    CHECK(add(&table, "host8.example", "10.0.0.7", "443", true,
              APPLIANCE_ROUTE_SOURCE_DECLARATION) == APPLIANCE_ROUTE_OK,
          "a freed slot accepts the next route");
    CHECK(table.count == APPLIANCE_ROUTE_TABLE_MAX,
          "the table is full again");
}

static void test_validate_hand_built(void)
{
    struct appliance_route_table table;

    memset(&table, 0, sizeof(table));
    table.version = APPLIANCE_ROUTE_TABLE_VERSION;
    CHECK(appliance_route_table_validate(&table) == APPLIANCE_ROUTE_OK,
          "a hand-built empty table validates");

    table.version = APPLIANCE_ROUTE_TABLE_VERSION + 1U;
    CHECK(appliance_route_table_validate(&table) ==
          APPLIANCE_ROUTE_BAD_VERSION,
          "a table with another version is refused");
    table.version = APPLIANCE_ROUTE_TABLE_VERSION;

    table.count = APPLIANCE_ROUTE_TABLE_MAX + 1U;
    CHECK(appliance_route_table_validate(&table) ==
          APPLIANCE_ROUTE_BAD_VERSION,
          "a table claiming more routes than it can hold is refused");
    table.count = 1U;

    table.routes[0].name_length = 0U;
    CHECK(appliance_route_table_validate(&table) == APPLIANCE_ROUTE_BAD_NAME,
          "a route without a name is refused");
    memcpy(table.routes[0].name, "example.com", strlen("example.com") + 1U);
    table.routes[0].name_length = (uint32_t)strlen("example.com");
    table.routes[0].backend[0] = 10U;
    table.routes[0].backend[3] = 1U;
    table.routes[0].backend_port = 443U;
    table.routes[0].source = (uint32_t)APPLIANCE_ROUTE_SOURCE_PANEL;
    CHECK(appliance_route_table_validate(&table) == APPLIANCE_ROUTE_OK,
          "a hand-built valid route validates");

    table.routes[0].backend_port = 0U;
    CHECK(appliance_route_table_validate(&table) == APPLIANCE_ROUTE_BAD_PORT,
          "a zero backend port is refused");
    table.routes[0].backend_port = 443U;

    table.routes[0].backend[0] = 127U;
    CHECK(appliance_route_table_validate(&table) ==
          APPLIANCE_ROUTE_BACKEND_NOT_HOST,
          "a loopback backend is refused");
    table.routes[0].backend[0] = 10U;

    table.routes[0].source = (uint32_t)APPLIANCE_ROUTE_SOURCE_NONE;
    CHECK(appliance_route_table_validate(&table) ==
          APPLIANCE_ROUTE_BAD_SOURCE,
          "a route without a producer is refused");
    table.routes[0].source = (uint32_t)APPLIANCE_ROUTE_SOURCE_PANEL;

    table.routes[0].name[0] = 'E';
    CHECK(appliance_route_table_validate(&table) == APPLIANCE_ROUTE_BAD_NAME,
          "a route whose stored name is not normalized is refused");
    table.routes[0].name[0] = 'e';

    table.count = 2U;
    memcpy(table.routes[1].name, "EXAMPLE.com", strlen("EXAMPLE.com") + 1U);
    table.routes[1].name_length = (uint32_t)strlen("EXAMPLE.com");
    table.routes[1].backend[0] = 10U;
    table.routes[1].backend[3] = 2U;
    table.routes[1].backend_port = 443U;
    table.routes[1].source = (uint32_t)APPLIANCE_ROUTE_SOURCE_PANEL;
    CHECK(appliance_route_table_validate(&table) == APPLIANCE_ROUTE_BAD_NAME,
          "a name that is not normalized is refused before the duplicate rule");
    memcpy(table.routes[1].name, "example.com", strlen("example.com") + 1U);
    CHECK(appliance_route_table_validate(&table) ==
          APPLIANCE_ROUTE_DUPLICATE_NAME,
          "a duplicated name is refused by the table");
}

static void test_render(void)
{
    struct appliance_route_table table;
    struct appliance_route route;
    char rendered[APPLIANCE_ROUTE_LIST_MAX];
    size_t length = 0U;
    size_t exact = 0U;

    appliance_route_table_init(&table);
    CHECK(add(&table, "example.com", "10.0.0.5", "8443", true,
              APPLIANCE_ROUTE_SOURCE_PANEL) == APPLIANCE_ROUTE_OK,
          "the rendered route is added");
    CHECK(appliance_route_table_lookup(&table, "example.com",
                                       strlen("example.com"),
                                       &route) == APPLIANCE_ROUTE_OK,
          "the rendered route is found");
    CHECK(appliance_route_render(&route, rendered, sizeof(rendered),
                                 &length) == APPLIANCE_ROUTE_OK,
          "one route renders");
    CHECK(strcmp(rendered,
                 "name=example.com backend=10.0.0.5:8443 tls=on source=PANEL")
              == 0,
          "the route record names the name, the backend, TLS and the source");
    exact = length;
    CHECK(appliance_route_render(&route, rendered, exact, &length) ==
          APPLIANCE_ROUTE_BUFFER_TOO_SMALL,
          "a buffer without room for the terminator is refused, not truncated");
    CHECK(appliance_route_render(NULL, rendered, sizeof(rendered), &length) ==
          APPLIANCE_ROUTE_NULL, "a null route is refused");

    CHECK(add(&table, "plain.example", "10.0.0.6", "80", false,
              APPLIANCE_ROUTE_SOURCE_DECLARATION) == APPLIANCE_ROUTE_OK,
          "the second rendered route is added");
    CHECK(appliance_route_table_render(&table, rendered, sizeof(rendered),
                                       &length) == APPLIANCE_ROUTE_OK,
          "the whole table renders");
    CHECK(strcmp(rendered,
                 "routes=2; name=example.com backend=10.0.0.5:8443 tls=on "
                 "source=PANEL; name=plain.example backend=10.0.0.6:80 "
                 "tls=off source=DECLARATION") == 0,
          "the table rendering carries every route in table order");
    CHECK(appliance_route_table_render(&table, rendered, length, &length) ==
          APPLIANCE_ROUTE_BUFFER_TOO_SMALL,
          "a table render that cannot fit the terminator is refused");
    CHECK(appliance_route_table_render(&table, rendered, 0U, &length) ==
          APPLIANCE_ROUTE_BUFFER_TOO_SMALL,
          "a zero-capacity table render is refused");
}

static void test_long_name_render(void)
{
    struct appliance_route_table table;
    struct appliance_route route;
    char name[APPLIANCE_ROUTE_NAME_MAX + 1U];
    char rendered[APPLIANCE_ROUTE_LIST_MAX];
    size_t written = 0U;
    size_t length = 0U;
    size_t part;

    for (part = 0U; part < 4U; ++part) {
        size_t label = part == 3U ? 61U : 63U;
        size_t index;

        if (part != 0U) {
            name[written] = '.';
            ++written;
        }
        for (index = 0U; index < label; ++index) {
            name[written] = 'a';
            ++written;
        }
    }
    name[written] = '\0';
    CHECK(written == APPLIANCE_ROUTE_NAME_MAX,
          "the longest name is exactly 253 bytes");

    appliance_route_table_init(&table);
    CHECK(add(&table, name, "10.0.0.9", "65535", false,
              APPLIANCE_ROUTE_SOURCE_DECLARATION) == APPLIANCE_ROUTE_OK,
          "the longest valid name is a route");
    CHECK(appliance_route_table_lookup(&table, name, written, &route) ==
          APPLIANCE_ROUTE_OK, "the longest name is found");
    CHECK(appliance_route_render(&route, rendered, sizeof(rendered),
                                 &length) == APPLIANCE_ROUTE_OK,
          "the longest name renders inside the record bound");
    CHECK(length <= APPLIANCE_ROUTE_RECORD_MAX && length > 192U,
          "the longest record is longer than the short bound yet still fits");
    CHECK(appliance_route_table_render(&table, rendered, sizeof(rendered),
                                       &length) == APPLIANCE_ROUTE_OK,
          "the longest table renders inside the list bound");
    CHECK(length <= APPLIANCE_ROUTE_LIST_MAX,
          "the longest table rendering is inside the declared bound");
}

static void test_strings(void)
{
    static const enum appliance_route_status statuses[] = {
        APPLIANCE_ROUTE_OK,
        APPLIANCE_ROUTE_NULL,
        APPLIANCE_ROUTE_BAD_VERSION,
        APPLIANCE_ROUTE_BAD_NAME,
        APPLIANCE_ROUTE_NAME_TOO_LONG,
        APPLIANCE_ROUTE_BAD_BACKEND,
        APPLIANCE_ROUTE_BACKEND_NOT_HOST,
        APPLIANCE_ROUTE_BAD_PORT,
        APPLIANCE_ROUTE_BAD_SOURCE,
        APPLIANCE_ROUTE_DUPLICATE_NAME,
        APPLIANCE_ROUTE_TABLE_FULL,
        APPLIANCE_ROUTE_NOT_FOUND,
        APPLIANCE_ROUTE_BUFFER_TOO_SMALL,
    };
    static const enum appliance_route_source sources[] = {
        APPLIANCE_ROUTE_SOURCE_NONE,
        APPLIANCE_ROUTE_SOURCE_DECLARATION,
        APPLIANCE_ROUTE_SOURCE_PANEL,
    };
    size_t index;

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]); ++index) {
        const char *text = appliance_route_status_string(statuses[index]);

        CHECK(text != NULL && text[0] != '\0',
              "every route status names itself");
    }
    for (index = 0U; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        const char *text = appliance_route_source_string(sources[index]);

        CHECK(text != NULL && text[0] != '\0',
              "every route source names itself");
    }
    CHECK(strcmp(appliance_route_source_string(
                     APPLIANCE_ROUTE_SOURCE_DECLARATION),
                 "DECLARATION") == 0,
          "the declaration source renders as its record token");
    CHECK(strcmp(appliance_route_source_string(APPLIANCE_ROUTE_SOURCE_PANEL),
                 "PANEL") == 0,
          "the panel source renders as its record token");
}

static void test_version_guard(void)
{
    struct appliance_route_table table;
    struct appliance_route route;

    appliance_route_table_init(&table);
    table.version = APPLIANCE_ROUTE_TABLE_VERSION + 1U;
    CHECK(add(&table, "example.com", "10.0.0.5", "8443", false,
              APPLIANCE_ROUTE_SOURCE_PANEL) == APPLIANCE_ROUTE_BAD_VERSION,
          "add refuses another table version");
    CHECK(appliance_route_table_remove(&table, "example.com",
                                       strlen("example.com")) ==
          APPLIANCE_ROUTE_BAD_VERSION,
          "remove refuses another table version");
    CHECK(appliance_route_table_lookup(&table, "example.com",
                                       strlen("example.com"),
                                       &route) == APPLIANCE_ROUTE_BAD_VERSION,
          "lookup refuses another table version");
    CHECK(appliance_route_table_add(NULL, "example.com",
                                    strlen("example.com"), "10.0.0.5", 8U,
                                    "8443", 4U, false,
                                    APPLIANCE_ROUTE_SOURCE_PANEL) ==
          APPLIANCE_ROUTE_NULL,
          "a null table is refused");
    CHECK(appliance_route_table_lookup(NULL, "example.com",
                                       strlen("example.com"),
                                       &route) == APPLIANCE_ROUTE_NULL,
          "a lookup without a table is refused");
}

int main(void)
{
    test_empty_table();
    test_add_and_lookup();
    test_duplicate_names();
    test_invalid_names();
    test_host_header_names();
    test_resolve_host();
    test_invalid_backends();
    test_invalid_ports();
    test_bad_source();
    test_table_full_and_remove();
    test_validate_hand_built();
    test_render();
    test_long_name_render();
    test_strings();
    test_version_guard();
    if (failures != 0) {
        fprintf(stderr, "appliance route table host test: %d FAILED\n",
                failures);
        return 1;
    }
    printf("appliance route table host test: ALL OK\n");
    return 0;
}
