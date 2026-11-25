/*
 * ITEM-504 part B host proof for the route candidate pair.
 *
 * The applied table must only ever change whole, and a stage the model refuses
 * must leave it exactly as it was.  This proof walks the two-phase contract
 * around the model: stage -> commit, stage -> rollback, a refused stage, a
 * malformed body and the action/flag grammar.
 */
#include "appliance_route_config.h"

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

static const uint8_t form_type[] = "application/x-www-form-urlencoded";

static enum appliance_route_config_status stage(
    struct appliance_route_config *config,
    const char *body)
{
    return appliance_route_config_stage(
        config, form_type, (uint32_t)(sizeof(form_type) - 1U),
        (const uint8_t *)body, (uint32_t)strlen(body));
}

static const struct appliance_route *only_route(
    const struct appliance_route_table *table)
{
    if (table == NULL || table->count != 1U) {
        return NULL;
    }
    return &table->routes[0];
}

static void test_init_and_empty_commit(void)
{
    struct appliance_route_config config;

    appliance_route_config_init(&config);
    CHECK(appliance_route_config_applied(&config) != NULL,
          "an initialized composition publishes an applied table");
    CHECK(appliance_route_config_applied(&config)->count == 0U,
          "the applied table starts empty");
    CHECK(!appliance_route_config_staged(&config),
          "nothing is staged after init");
    CHECK(appliance_route_config_commit(&config) ==
          APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE,
          "a commit without a candidate is refused");
    CHECK(appliance_route_config_rollback(&config) ==
          APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE,
          "a rollback without a candidate is refused");
    CHECK(appliance_route_config_commit_count(&config) == 0U,
          "a refused commit increments no commit count");
}

static void test_stage_commit_add(void)
{
    struct appliance_route_config config;
    const struct appliance_route_table *applied;
    const struct appliance_route *route;

    appliance_route_config_init(&config);
    CHECK(stage(&config, "name=example.com&backend=10.0.0.5&port=8443&tls=on")
              == APPLIANCE_ROUTE_CONFIG_OK,
          "a complete add stages");
    CHECK(appliance_route_config_staged(&config),
          "the candidate is held after a stage");
    CHECK(appliance_route_config_applied(&config)->count == 0U,
          "staging changes nothing in the applied table");
    CHECK(appliance_route_config_commit(&config) ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "the staged candidate commits");
    applied = appliance_route_config_applied(&config);
    route = only_route(applied);
    CHECK(route != NULL, "the applied table carries the committed route");
    CHECK(route != NULL && route->backend_port == 8443U,
          "the committed route carries the port");
    CHECK(route != NULL && route->tls_to_backend,
          "the committed route carries the TLS flag");
    CHECK(route != NULL &&
          route->source == (uint32_t)APPLIANCE_ROUTE_SOURCE_PANEL,
          "a committed route is a panel route");
    CHECK(!appliance_route_config_staged(&config),
          "the candidate is gone after a commit");
    CHECK(appliance_route_config_commit_count(&config) == 1U,
          "the commit is counted");
    CHECK(appliance_route_config_stage_count(&config) == 1U,
          "the stage is counted");
}

static void test_stage_rollback(void)
{
    struct appliance_route_config config;
    const struct appliance_route *route;

    appliance_route_config_init(&config);
    CHECK(stage(&config, "name=example.com&backend=10.0.0.5&port=8443") ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "an add without tls stages");
    CHECK(appliance_route_config_rollback(&config) ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "the candidate rolls back");
    CHECK(!appliance_route_config_staged(&config),
          "nothing is staged after a rollback");
    CHECK(appliance_route_config_applied(&config)->count == 0U,
          "a rolled-back stage never reaches the applied table");
    CHECK(appliance_route_config_rollback_count(&config) == 1U,
          "the rollback is counted");
    CHECK(appliance_route_config_commit_count(&config) == 0U,
          "a rollback is not a commit");
    route = only_route(appliance_route_config_applied(&config));
    CHECK(route == NULL, "the applied table is still empty");
}

static void test_stage_replaces_candidate(void)
{
    struct appliance_route_config config;
    const struct appliance_route *route;

    appliance_route_config_init(&config);
    CHECK(stage(&config, "name=first.example&backend=10.0.0.1&port=80") ==
          APPLIANCE_ROUTE_CONFIG_OK, "the first candidate stages");
    CHECK(stage(&config, "name=second.example&backend=10.0.0.2&port=81") ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "a second stage replaces the candidate");
    CHECK(appliance_route_config_commit(&config) ==
          APPLIANCE_ROUTE_CONFIG_OK, "the second candidate commits");
    route = only_route(appliance_route_config_applied(&config));
    CHECK(route != NULL && route->name_length == strlen("second.example") &&
          memcmp(route->name, "second.example", strlen("second.example")) == 0,
          "only the last staged candidate is applied");
    CHECK(appliance_route_config_applied(&config)->count == 1U,
          "the applied table holds exactly one route");
}

static void test_remove_route(void)
{
    struct appliance_route_config config;

    appliance_route_config_init(&config);
    CHECK(stage(&config, "name=example.com&backend=10.0.0.5&port=8443") ==
          APPLIANCE_ROUTE_CONFIG_OK, "the route to remove stages");
    CHECK(appliance_route_config_commit(&config) ==
          APPLIANCE_ROUTE_CONFIG_OK, "the route is applied");
    CHECK(stage(&config, "action=remove&name=EXAMPLE.COM.") ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "a remove stages by normalized name");
    CHECK(appliance_route_config_applied(&config)->count == 1U,
          "staging a remove does not remove from the applied table");
    CHECK(appliance_route_config_commit(&config) ==
          APPLIANCE_ROUTE_CONFIG_OK, "the remove commits");
    CHECK(appliance_route_config_applied(&config)->count == 0U,
          "the committed remove empties the applied table");
    CHECK(stage(&config, "action=remove&name=example.com") ==
          APPLIANCE_ROUTE_CONFIG_REFUSED,
          "removing a name the table does not carry is refused");
    CHECK(appliance_route_config_last_route_status(&config) ==
          APPLIANCE_ROUTE_NOT_FOUND,
          "the refusal names NOT_FOUND from the model");
    CHECK(appliance_route_config_refused_count(&config) == 1U,
          "the refusal is counted");
    /*
     * ITEM-647: the old browser body carried `backend=`, `port=` and `tls=`
     * with empty values on a remove, and the parser rejected the extra fields
     * (`MALFORMED`).  The rendered remove form now sends only action and name;
     * the parser keeps refusing the old shape, including empty extras, so a
     * stale client cannot smuggle a field into the remove contract.
     */
    CHECK(stage(&config,
                "action=remove&name=example.com&backend=&port=&tls=") ==
          APPLIANCE_ROUTE_CONFIG_MALFORMED,
          "the old remove body with empty extra fields stays refused");
    CHECK(appliance_route_config_applied(&config)->count == 0U &&
              appliance_route_config_last_status(&config) ==
                  APPLIANCE_ROUTE_CONFIG_MALFORMED,
          "the refused old body mutates nothing and is named MALFORMED");
}

static void test_refused_stage_keeps_applied(void)
{
    struct appliance_route_config config;

    appliance_route_config_init(&config);
    CHECK(stage(&config, "name=example.com&backend=10.0.0.5&port=8443") ==
          APPLIANCE_ROUTE_CONFIG_OK, "the first route stages");
    CHECK(appliance_route_config_commit(&config) ==
          APPLIANCE_ROUTE_CONFIG_OK, "the first route applies");
    CHECK(stage(&config, "name=EXAMPLE.com&backend=10.0.0.9&port=443") ==
          APPLIANCE_ROUTE_CONFIG_REFUSED,
          "a duplicate name is refused by the model");
    CHECK(appliance_route_config_last_route_status(&config) ==
          APPLIANCE_ROUTE_DUPLICATE_NAME,
          "the refusal names DUPLICATE_NAME");
    CHECK(!appliance_route_config_staged(&config),
          "a refused stage holds no candidate");
    CHECK(appliance_route_config_applied(&config)->count == 1U,
          "a refused stage keeps the applied table");
    CHECK(appliance_route_config_applied(&config)->routes[0].backend[3] == 5U,
          "a refused stage keeps the applied route bytes");

    CHECK(stage(&config, "name=bad.example&backend=127.0.0.1&port=80") ==
          APPLIANCE_ROUTE_CONFIG_REFUSED,
          "a loopback backend is refused by the model");
    CHECK(appliance_route_config_last_route_status(&config) ==
          APPLIANCE_ROUTE_BACKEND_NOT_HOST,
          "the refusal names BACKEND_NOT_HOST");
    CHECK(stage(&config, "name=bad..name&backend=10.0.0.1&port=80") ==
          APPLIANCE_ROUTE_CONFIG_REFUSED,
          "a malformed name is refused by the model");
    CHECK(appliance_route_config_last_route_status(&config) ==
          APPLIANCE_ROUTE_BAD_NAME, "the refusal names BAD_NAME");
    CHECK(stage(&config, "name=x.example&backend=10.0.0.1&port=0") ==
          APPLIANCE_ROUTE_CONFIG_REFUSED, "a zero port is refused by the model");
    CHECK(appliance_route_config_last_route_status(&config) ==
          APPLIANCE_ROUTE_BAD_PORT, "the refusal names BAD_PORT");
    CHECK(appliance_route_config_applied(&config)->count == 1U,
          "no model refusal changed the applied table");
}

static void test_table_full(void)
{
    struct appliance_route_config config;
    char body[128];
    uint32_t index;

    appliance_route_config_init(&config);
    for (index = 0U; index < APPLIANCE_ROUTE_TABLE_MAX; ++index) {
        snprintf(body, sizeof(body), "name=host%u.example&backend=10.0.0.5"
                                     "&port=8443", (unsigned int)index);
        CHECK(stage(&config, body) == APPLIANCE_ROUTE_CONFIG_OK,
              "a route fits while the table has room");
        CHECK(appliance_route_config_commit(&config) ==
              APPLIANCE_ROUTE_CONFIG_OK, "each candidate commits");
    }
    snprintf(body, sizeof(body), "name=overflow.example&backend=10.0.0.5"
                                 "&port=8443");
    CHECK(stage(&config, body) == APPLIANCE_ROUTE_CONFIG_REFUSED,
          "a ninth route is refused by the model");
    CHECK(appliance_route_config_last_route_status(&config) ==
          APPLIANCE_ROUTE_TABLE_FULL, "the refusal names TABLE_FULL");
    CHECK(appliance_route_config_applied(&config)->count ==
          APPLIANCE_ROUTE_TABLE_MAX,
          "the applied table stays at the limit");
}

static void test_malformed_bodies(void)
{
    struct appliance_route_config config;
    struct {
        const char *body;
        enum appliance_route_config_status status;
    } cases[] = {
        { "name", APPLIANCE_ROUTE_CONFIG_MALFORMED },
        { "name=example.com&name=other.example&backend=10.0.0.5&port=80",
          APPLIANCE_ROUTE_CONFIG_MALFORMED },
        { "name=example.com&unknown=1", APPLIANCE_ROUTE_CONFIG_MALFORMED },
        { "=example.com", APPLIANCE_ROUTE_CONFIG_MALFORMED },
        { "name=example.com&backend=10.0.0.5&port=123456",
          APPLIANCE_ROUTE_CONFIG_MALFORMED },
        { "backend=10.0.0.5&port=80",
          APPLIANCE_ROUTE_CONFIG_MISSING_FIELD },
        { "name=example.com&port=80", APPLIANCE_ROUTE_CONFIG_MISSING_FIELD },
        { "name=example.com&backend=10.0.0.5",
          APPLIANCE_ROUTE_CONFIG_MISSING_FIELD },
        { "name=example.com&backend=10.0.0.5&port=80&action=delete",
          APPLIANCE_ROUTE_CONFIG_BAD_ACTION },
        { "name=example.com&backend=10.0.0.5&port=80&tls=yes",
          APPLIANCE_ROUTE_CONFIG_BAD_FLAG },
        { "name=example.com&backend=10.0.0.5&port=80&tls=maybe",
          APPLIANCE_ROUTE_CONFIG_MALFORMED },
        { "action=remove&name=example.com&backend=10.0.0.5",
          APPLIANCE_ROUTE_CONFIG_MALFORMED },
        { "action=remove&name=example.com&tls=on",
          APPLIANCE_ROUTE_CONFIG_MALFORMED },
        { "name=example.com&backend=10.0.0.5&port=80&action=add&tls=off",
          APPLIANCE_ROUTE_CONFIG_OK },
        { "name=example.com&backend=10.0.0.5&port=80&action=",
          APPLIANCE_ROUTE_CONFIG_BAD_ACTION },
    };
    size_t index;

    appliance_route_config_init(&config);
    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        char message[160];

        snprintf(message, sizeof(message), "the body `%s` is decided as declared",
                 cases[index].body);
        CHECK(stage(&config, cases[index].body) == cases[index].status,
              message);
    }
    CHECK(appliance_route_config_applied(&config)->count == 0U,
          "no malformed body reached the applied table");
}

static void test_content_type_and_bounds(void)
{
    struct appliance_route_config config;
    static const uint8_t wrong_type[] = "text/plain";
    char body[APPLIANCE_ROUTE_CONFIG_BODY_MAX + 1U];

    appliance_route_config_init(&config);
    CHECK(appliance_route_config_stage(
              &config, wrong_type, (uint32_t)(sizeof(wrong_type) - 1U),
              (const uint8_t *)"name=x.example", strlen("name=x.example")) ==
          APPLIANCE_ROUTE_CONFIG_BAD_CONTENT_TYPE,
          "another content type is refused");
    CHECK(appliance_route_config_stage(
              &config, form_type, (uint32_t)(sizeof(form_type) - 1U),
              (const uint8_t *)"", 0U) == APPLIANCE_ROUTE_CONFIG_MALFORMED,
          "an empty body is refused");
    memset(body, 'a', sizeof(body));
    CHECK(appliance_route_config_stage(
              &config, form_type, (uint32_t)(sizeof(form_type) - 1U),
              (const uint8_t *)body,
              APPLIANCE_ROUTE_CONFIG_BODY_MAX + 1U) ==
          APPLIANCE_ROUTE_CONFIG_MALFORMED,
          "a body over the framing bound is refused, not truncated");
    CHECK(appliance_route_config_stage(NULL, form_type,
                                       (uint32_t)(sizeof(form_type) - 1U),
                                       (const uint8_t *)"name=x.example",
                                       strlen("name=x.example")) ==
          APPLIANCE_ROUTE_CONFIG_NULL, "a null composition is refused");
    CHECK(appliance_route_config_last_route_status(NULL) ==
          APPLIANCE_ROUTE_BAD_VERSION, "a null composition has no route status");
}

static void test_version_guard(void)
{
    struct appliance_route_config config;

    appliance_route_config_init(&config);
    config.version = APPLIANCE_ROUTE_CONFIG_VERSION + 1U;
    CHECK(stage(&config, "name=example.com&backend=10.0.0.5&port=80") ==
          APPLIANCE_ROUTE_CONFIG_BAD_VERSION,
          "a stage refuses another composition version");
    CHECK(appliance_route_config_commit(&config) ==
          APPLIANCE_ROUTE_CONFIG_BAD_VERSION,
          "a commit refuses another composition version");
}

static void test_status_strings(void)
{
    static const enum appliance_route_config_status statuses[] = {
        APPLIANCE_ROUTE_CONFIG_OK,
        APPLIANCE_ROUTE_CONFIG_NULL,
        APPLIANCE_ROUTE_CONFIG_BAD_VERSION,
        APPLIANCE_ROUTE_CONFIG_BAD_CONTENT_TYPE,
        APPLIANCE_ROUTE_CONFIG_MALFORMED,
        APPLIANCE_ROUTE_CONFIG_MISSING_FIELD,
        APPLIANCE_ROUTE_CONFIG_BAD_ACTION,
        APPLIANCE_ROUTE_CONFIG_BAD_FLAG,
        APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE,
        APPLIANCE_ROUTE_CONFIG_REFUSED,
    };
    size_t index;

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]); ++index) {
        const char *text = appliance_route_config_status_string(statuses[index]);

        CHECK(text != NULL && text[0] != '\0',
              "every route-config status names itself");
    }
}

/*
 * ITEM-504 increment 5: the records.  A terminal state leaves one bounded body,
 * the machine drains it exactly once, and a refusal keeps the model's own
 * reason instead of a generic failure.
 */
static void test_record_applied(void)
{
    struct appliance_route_config config;
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    size_t record_length = 0U;

    appliance_route_config_init(&config);
    CHECK(appliance_route_config_record_result(&config) ==
          APPLIANCE_ROUTE_CONFIG_RECORD_NONE,
          "an initialized composition left no record");
    CHECK(!appliance_route_config_take_record(&config, record,
                                              sizeof(record),
                                              &record_length),
          "a composition with no terminal state hands over no record");
    CHECK(stage(&config,
                "name=Example.COM.&backend=10.0.0.5&port=8443&tls=on") ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "the record route stages");
    CHECK(!appliance_route_config_take_record(&config, record,
                                              sizeof(record),
                                              &record_length),
          "a held candidate leaves no record until it is terminal");
    CHECK(appliance_route_config_commit(&config) == APPLIANCE_ROUTE_CONFIG_OK,
          "the record route commits");
    memset(record, 0, sizeof(record));
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length),
          "the commit leaves a record");
    CHECK(strcmp(record,
                 "name=example.com backend=10.0.0.5:8443 tls=on "
                 "source=PANEL result=APPLIED") == 0,
          "the record names the applied route and its result");
    CHECK(record_length == strlen(record),
          "the record length excludes the terminator");
    CHECK(appliance_route_config_record_result(&config) ==
          APPLIANCE_ROUTE_CONFIG_RECORD_APPLIED,
          "the record result is APPLIED");
    CHECK(appliance_route_config_record_count(&config) == 1U,
          "the produced record is counted");
    CHECK(appliance_route_config_record_result_string(
              APPLIANCE_ROUTE_CONFIG_RECORD_APPLIED) != NULL,
          "the APPLIED result names itself");
    CHECK(!appliance_route_config_take_record(&config, record,
                                              sizeof(record),
                                              &record_length),
          "the record is handed over exactly once");
}

static void test_record_cancelled(void)
{
    struct appliance_route_config config;
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    size_t record_length = 0U;

    appliance_route_config_init(&config);
    CHECK(stage(&config,
                "name=example.com&backend=10.0.0.5&port=8443&tls=on") ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "a route stages before the cancel");
    CHECK(appliance_route_config_commit(&config) == APPLIANCE_ROUTE_CONFIG_OK,
          "the route applies before the cancel");
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length),
          "the applied record is drained before the cancel");
    CHECK(stage(&config, "action=remove&name=EXAMPLE.COM.") ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "the remove stages");
    CHECK(appliance_route_config_rollback(&config) ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "the candidate is cancelled");
    memset(record, 0, sizeof(record));
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length),
          "a cancel leaves a record");
    CHECK(strcmp(record,
                 "name=example.com backend=10.0.0.5:8443 tls=on "
                 "source=PANEL result=CANCELLED") == 0,
          "the record names the dropped route and its result");
    CHECK(appliance_route_config_record_result(&config) ==
          APPLIANCE_ROUTE_CONFIG_RECORD_CANCELLED,
          "the record result is CANCELLED");
    CHECK(!appliance_route_config_take_record(&config, record,
                                              sizeof(record),
                                              &record_length),
          "the cancelled record is handed over exactly once");
}

static void test_record_refusal(void)
{
    struct appliance_route_config config;
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    size_t record_length = 0U;

    appliance_route_config_init(&config);
    CHECK(stage(&config,
                "name=example.com&backend=10.0.0.5&port=8443&tls=on") ==
          APPLIANCE_ROUTE_CONFIG_OK,
          "the first record route stages");
    CHECK(appliance_route_config_commit(&config) == APPLIANCE_ROUTE_CONFIG_OK,
          "the first record route applies");
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length),
          "the applied record is drained before the refusal");
    CHECK(stage(&config, "name=EXAMPLE.com&backend=10.0.0.9&port=443") ==
          APPLIANCE_ROUTE_CONFIG_REFUSED,
          "a duplicate name is refused");
    /* A rollback with no candidate must not overwrite the refusal. */
    CHECK(appliance_route_config_rollback(&config) ==
          APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE,
          "a rollback after a refusal is refused");
    memset(record, 0, sizeof(record));
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length),
          "the refusal leaves a record");
    CHECK(strcmp(record,
                 "name=EXAMPLE.com result=REFUSED reason=DUPLICATE_NAME") == 0,
          "the refusal names the requested name and the model's reason");
    CHECK(appliance_route_config_record_result(&config) ==
          APPLIANCE_ROUTE_CONFIG_RECORD_REFUSED,
          "the record result is REFUSED");
    CHECK(strcmp(appliance_route_config_record_result_string(
                     APPLIANCE_ROUTE_CONFIG_RECORD_REFUSED),
                 "REFUSED") == 0,
          "the REFUSED result names itself");

    CHECK(stage(&config, "name=bad.example&backend=127.0.0.1&port=80") ==
          APPLIANCE_ROUTE_CONFIG_REFUSED,
          "a loopback backend is refused");
    memset(record, 0, sizeof(record));
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length) &&
          strcmp(record,
                 "name=bad.example result=REFUSED "
                 "reason=BACKEND_NOT_HOST") == 0,
          "the refusal names the backend reason");
}

static void test_record_refusal_without_a_model(void)
{
    struct appliance_route_config config;
    static const uint8_t wrong_type[] = "text/plain";
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    size_t record_length = 0U;

    appliance_route_config_init(&config);
    CHECK(appliance_route_config_stage(
              &config, wrong_type, (uint32_t)(sizeof(wrong_type) - 1U),
              (const uint8_t *)"name=x.example",
              strlen("name=x.example")) ==
          APPLIANCE_ROUTE_CONFIG_BAD_CONTENT_TYPE,
          "another content type is refused before the model");
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length) &&
          strcmp(record, "result=REFUSED reason=BAD_CONTENT_TYPE") == 0,
          "a pre-model refusal names its own status without a name");

    CHECK(stage(&config, "name=example.com&unknown=1") ==
          APPLIANCE_ROUTE_CONFIG_MALFORMED,
          "an unknown field is refused by the parser");
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length) &&
          strcmp(record,
                 "name=example.com result=REFUSED reason=MALFORMED") == 0,
          "a parser refusal names the field the body carried");

    /* A hostile name must not be able to start a second log line. */
    CHECK(stage(&config, "name=evil\nname&backend=10.0.0.5&port=80") ==
          APPLIANCE_ROUTE_CONFIG_REFUSED,
          "a name carrying a newline is refused by the model");
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length) &&
          strcmp(record,
                 "name=evil?name result=REFUSED reason=BAD_NAME") == 0,
          "a hostile name is echoed with its control byte made visible");
}

static void test_record_short_buffer_keeps_it(void)
{
    struct appliance_route_config config;
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    size_t record_length = 0U;

    appliance_route_config_init(&config);
    CHECK(stage(&config, "name=example.com&backend=10.0.0.5&port=80") ==
          APPLIANCE_ROUTE_CONFIG_OK, "the short-buffer route stages");
    CHECK(appliance_route_config_commit(&config) == APPLIANCE_ROUTE_CONFIG_OK,
          "the short-buffer route commits");
    CHECK(!appliance_route_config_take_record(&config, record, 8U,
                                              &record_length),
          "a buffer that cannot hold the whole record takes nothing");
    CHECK(appliance_route_config_record_result(&config) ==
          APPLIANCE_ROUTE_CONFIG_RECORD_APPLIED,
          "a refused take drops nothing");
    memset(record, 0, sizeof(record));
    CHECK(appliance_route_config_take_record(&config, record,
                                             sizeof(record),
                                             &record_length) &&
          strcmp(record,
                 "name=example.com backend=10.0.0.5:80 tls=off "
                 "source=PANEL result=APPLIED") == 0,
          "the whole record is still there for a larger buffer");
    CHECK(!appliance_route_config_take_record(NULL, record,
                                              sizeof(record),
                                              &record_length),
          "a null composition takes no record");
    CHECK(!appliance_route_config_take_record(&config, NULL,
                                              sizeof(record),
                                              &record_length),
          "a null buffer takes no record");
    CHECK(!appliance_route_config_take_record(&config, record,
                                              sizeof(record), NULL),
          "a null length takes no record");
    CHECK(appliance_route_config_record_result(NULL) ==
          APPLIANCE_ROUTE_CONFIG_RECORD_NONE,
          "a null composition has no record result");
    CHECK(appliance_route_config_record_count(NULL) == 0U,
          "a null composition counted no record");
    CHECK(appliance_route_config_record_result_string(
              APPLIANCE_ROUTE_CONFIG_RECORD_NONE) != NULL,
          "the NONE result names itself");
}

int main(void)
{
    test_init_and_empty_commit();
    test_stage_commit_add();
    test_stage_rollback();
    test_stage_replaces_candidate();
    test_remove_route();
    test_refused_stage_keeps_applied();
    test_table_full();
    test_malformed_bodies();
    test_content_type_and_bounds();
    test_version_guard();
    test_status_strings();
    test_record_applied();
    test_record_cancelled();
    test_record_refusal();
    test_record_refusal_without_a_model();
    test_record_short_buffer_keeps_it();
    if (failures != 0) {
        fprintf(stderr, "appliance route config host test: %d FAILED\n",
                failures);
        return 1;
    }
    printf("appliance route config host test: ALL OK\n");
    return 0;
}
