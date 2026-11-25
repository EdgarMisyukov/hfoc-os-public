#include "appliance_route_config.h"

#include <string.h>

/*
 * ITEM-504 part B: the candidate pair behind the ROUTING_CONFIG mutation.
 *
 * A stage never touches the applied table: it copies the applied table inside
 * the composition, applies exactly one validated add or remove to that copy,
 * and holds it.  A commit replaces the applied table with the candidate whole;
 * a rollback or a refusal drops the candidate.  Every refusal is the model's
 * own named refusal (the route table owns the grammar), kept in
 * `last_route_status` so a record can state why nothing changed.
 *
 * ITEM-504 increment 5: a terminal state also leaves one bounded record body
 * in the composition's single record slot, and the ordinary boot drains it as
 * `[PRODUCTION-ROUTE]`.  The record is built here, next to the state that
 * decided it, and never invented by the writer.
 */

struct appliance_route_config_fields {
    char name[APPLIANCE_ROUTE_CONFIG_NAME_MAX + 1U];
    size_t name_length;
    bool name_set;
    char backend[APPLIANCE_ROUTE_CONFIG_BACKEND_MAX + 1U];
    size_t backend_length;
    bool backend_set;
    char port[APPLIANCE_ROUTE_CONFIG_PORT_MAX + 1U];
    size_t port_length;
    bool port_set;
    char flag[APPLIANCE_ROUTE_CONFIG_FLAG_MAX + 1U];
    size_t flag_length;
    bool flag_set;
    char action[APPLIANCE_ROUTE_CONFIG_ACTION_MAX + 1U];
    size_t action_length;
    bool action_set;
};

static bool route_config_field_is(
    const uint8_t *name,
    size_t name_length,
    const char *expected)
{
    size_t expected_length = 0U;
    size_t index;

    while (expected[expected_length] != '\0') {
        ++expected_length;
    }
    if (name_length != expected_length) {
        return false;
    }
    for (index = 0U; index < name_length; ++index) {
        if (name[index] != (uint8_t)expected[index]) {
            return false;
        }
    }
    return true;
}

static bool route_config_value_is(
    const char *value,
    size_t value_length,
    const char *expected)
{
    size_t expected_length = 0U;

    while (expected[expected_length] != '\0') {
        ++expected_length;
    }
    if (value_length != expected_length) {
        return false;
    }
    return memcmp(value, expected, value_length) == 0;
}

static bool route_config_copy_value(
    char *destination,
    size_t capacity,
    bool *set,
    const uint8_t *value,
    size_t value_length,
    size_t *length_out)
{
    if (*set || value_length >= capacity) {
        return false;
    }
    memcpy(destination, value, value_length);
    destination[value_length] = '\0';
    *set = true;
    *length_out = value_length;
    return true;
}

/*
 * The body is a sequence of `field=value` pairs separated by `&`.  Duplicates,
 * an unknown field, a missing `=`, an empty field name and a value over its
 * field bound are all refused: an ambiguous body never reaches the model.
 */
static enum appliance_route_config_status route_config_parse_body(
    const uint8_t *body,
    uint32_t body_length,
    struct appliance_route_config_fields *fields)
{
    uint32_t index = 0U;

    if (body == NULL || fields == NULL || body_length == 0U) {
        return APPLIANCE_ROUTE_CONFIG_MALFORMED;
    }
    memset(fields, 0, sizeof(*fields));
    while (index < body_length) {
        uint32_t field_end = index;
        uint32_t separator = index;
        const uint8_t *name;
        const uint8_t *value;
        size_t name_length;
        size_t value_length;

        while (field_end < body_length && body[field_end] != (uint8_t)'&') {
            ++field_end;
        }
        while (separator < field_end && body[separator] != (uint8_t)'=') {
            ++separator;
        }
        if (separator >= field_end) {
            return APPLIANCE_ROUTE_CONFIG_MALFORMED;
        }
        name = body + index;
        name_length = (size_t)(separator - index);
        value = body + separator + 1U;
        value_length = (size_t)(field_end - separator - 1U);
        if (name_length == 0U) {
            return APPLIANCE_ROUTE_CONFIG_MALFORMED;
        }
        if (route_config_field_is(name, name_length, "name")) {
            if (!route_config_copy_value(fields->name, sizeof(fields->name),
                                         &fields->name_set, value,
                                         value_length,
                                         &fields->name_length)) {
                return APPLIANCE_ROUTE_CONFIG_MALFORMED;
            }
        } else if (route_config_field_is(name, name_length, "backend")) {
            if (!route_config_copy_value(fields->backend,
                                         sizeof(fields->backend),
                                         &fields->backend_set, value,
                                         value_length,
                                         &fields->backend_length)) {
                return APPLIANCE_ROUTE_CONFIG_MALFORMED;
            }
        } else if (route_config_field_is(name, name_length, "port")) {
            if (!route_config_copy_value(fields->port, sizeof(fields->port),
                                         &fields->port_set, value,
                                         value_length,
                                         &fields->port_length)) {
                return APPLIANCE_ROUTE_CONFIG_MALFORMED;
            }
        } else if (route_config_field_is(name, name_length, "tls")) {
            if (!route_config_copy_value(fields->flag, sizeof(fields->flag),
                                         &fields->flag_set, value,
                                         value_length,
                                         &fields->flag_length)) {
                return APPLIANCE_ROUTE_CONFIG_MALFORMED;
            }
        } else if (route_config_field_is(name, name_length, "action")) {
            if (!route_config_copy_value(fields->action,
                                         sizeof(fields->action),
                                         &fields->action_set, value,
                                         value_length,
                                         &fields->action_length)) {
                return APPLIANCE_ROUTE_CONFIG_MALFORMED;
            }
        } else {
            return APPLIANCE_ROUTE_CONFIG_MALFORMED;
        }
        index = field_end + 1U;
    }
    return APPLIANCE_ROUTE_CONFIG_OK;
}

/*
 * ITEM-504 increment 5: the record slot.  Bodies are ASCII lines a reader can
 * print as they are.  A name the model refused was not normalized, so every
 * byte outside the visible range becomes `?`: a hostile value must not be able
 * to end the record or start a new one.  Nothing is published if the whole
 * body would not fit, so a record is never a truncated one.
 */
static void route_config_record_append(
    char *record,
    size_t capacity,
    size_t *length,
    const char *text,
    size_t text_length)
{
    if (*length >= capacity ||
        text_length > capacity - 1U - *length) {
        *length = capacity; /* out of range: the caller refuses to publish */
        return;
    }
    memcpy(record + *length, text, text_length);
    *length += text_length;
    record[*length] = '\0';
}

static void route_config_record_append_word(
    char *record,
    size_t capacity,
    size_t *length,
    const char *word)
{
    route_config_record_append(record, capacity, length, word,
                               strlen(word));
}

static void route_config_record_append_name(
    char *record,
    size_t capacity,
    size_t *length,
    const char *name,
    size_t name_length)
{
    size_t index;

    route_config_record_append_word(record, capacity, length, "name=");
    if (name == NULL || name_length == 0U) {
        route_config_record_append_word(record, capacity, length, "UNKNOWN");
        return;
    }
    for (index = 0U; index < name_length; ++index) {
        const unsigned char byte = (unsigned char)name[index];
        const char visible = byte >= 0x21U && byte <= 0x7EU
            ? (char)byte
            : '?';

        route_config_record_append(record, capacity, length, &visible, 1U);
    }
}

static void route_config_record_publish(
    struct appliance_route_config *config,
    enum appliance_route_config_record_result result,
    const char *record,
    size_t record_length)
{
    if (record_length >= sizeof(config->record) ||
        record_length > APPLIANCE_ROUTE_CONFIG_RECORD_MAX) {
        return; /* the record is bounded; never publish a partial one */
    }
    memcpy(config->record, record, record_length);
    config->record[record_length] = '\0';
    config->record_length = record_length;
    config->record_pending = true;
    config->record_result = result;
    ++config->record_count;
}

/* `result=APPLIED` or `result=CANCELLED`, on the remembered operation. */
static void route_config_record_operation(
    struct appliance_route_config *config,
    enum appliance_route_config_record_result result,
    const char *result_word)
{
    char body[APPLIANCE_ROUTE_RECORD_MAX];
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    size_t body_length = 0U;
    size_t record_length = 0U;

    if (!config->operation_set ||
        appliance_route_render(&config->operation, body, sizeof(body),
                               &body_length) != APPLIANCE_ROUTE_OK) {
        return; /* nothing was remembered: a record would be a fabrication */
    }
    record[0] = '\0';
    route_config_record_append(record, sizeof(record), &record_length, body,
                               body_length);
    route_config_record_append_word(record, sizeof(record), &record_length,
                                    " result=");
    route_config_record_append_word(record, sizeof(record), &record_length,
                                    result_word);
    route_config_record_publish(config, result, record, record_length);
}

/*
 * A refusal applied nothing, so the record names the requested name (when the
 * body carried one) and the state that refused it: the model's own reason for
 * a route the grammar judged, the composition's status otherwise.
 */
static void route_config_record_refusal(
    struct appliance_route_config *config,
    const struct appliance_route_config_fields *fields,
    const char *reason)
{
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    size_t record_length = 0U;

    record[0] = '\0';
    if (fields != NULL && fields->name_set) {
        route_config_record_append_name(record, sizeof(record),
                                        &record_length, fields->name,
                                        fields->name_length);
        route_config_record_append(record, sizeof(record), &record_length,
                                   " ", 1U);
    }
    route_config_record_append_word(record, sizeof(record), &record_length,
                                    "result=REFUSED reason=");
    route_config_record_append_word(record, sizeof(record), &record_length,
                                    reason);
    route_config_record_publish(config, APPLIANCE_ROUTE_CONFIG_RECORD_REFUSED,
                                record, record_length);
}

void appliance_route_config_init(struct appliance_route_config *config)
{
    if (config == NULL) {
        return;
    }
    config->initialized = true;
    config->version = APPLIANCE_ROUTE_CONFIG_VERSION;
    config->staged = false;
    appliance_route_table_init(&config->applied);
    appliance_route_table_init(&config->candidate);
    config->last_status = APPLIANCE_ROUTE_CONFIG_OK;
    config->last_route_status = APPLIANCE_ROUTE_OK;
    config->stage_count = 0U;
    config->commit_count = 0U;
    config->rollback_count = 0U;
    config->refused_count = 0U;
    config->operation_set = false;
    memset(&config->operation, 0, sizeof(config->operation));
    config->record[0] = '\0';
    config->record_length = 0U;
    config->record_pending = false;
    config->record_result = APPLIANCE_ROUTE_CONFIG_RECORD_NONE;
    config->record_count = 0U;
}

enum appliance_route_config_status appliance_route_config_stage(
    struct appliance_route_config *config,
    const uint8_t *content_type,
    uint32_t content_type_length,
    const uint8_t *body,
    uint32_t body_length)
{
    struct appliance_route_config_fields fields;
    enum appliance_route_status route_status = APPLIANCE_ROUTE_OK;
    enum appliance_route_config_status status;
    bool remove;

    if (config == NULL) {
        return APPLIANCE_ROUTE_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->version != APPLIANCE_ROUTE_CONFIG_VERSION) {
        return APPLIANCE_ROUTE_CONFIG_BAD_VERSION;
    }
    if (content_type == NULL || body == NULL) {
        return APPLIANCE_ROUTE_CONFIG_NULL;
    }
    if (content_type_length != APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE_LENGTH ||
        memcmp(content_type, APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE,
               APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE_LENGTH) != 0) {
        config->last_status = APPLIANCE_ROUTE_CONFIG_BAD_CONTENT_TYPE;
        route_config_record_refusal(
            config, NULL,
            appliance_route_config_status_string(config->last_status));
        return config->last_status;
    }
    if (body_length == 0U || body_length > APPLIANCE_ROUTE_CONFIG_BODY_MAX) {
        config->last_status = APPLIANCE_ROUTE_CONFIG_MALFORMED;
        route_config_record_refusal(
            config, NULL,
            appliance_route_config_status_string(config->last_status));
        return config->last_status;
    }
    status = route_config_parse_body(body, body_length, &fields);
    if (status != APPLIANCE_ROUTE_CONFIG_OK) {
        config->last_status = status;
        route_config_record_refusal(
            config, &fields,
            appliance_route_config_status_string(config->last_status));
        return status;
    }
    if (!fields.name_set) {
        config->last_status = APPLIANCE_ROUTE_CONFIG_MISSING_FIELD;
        route_config_record_refusal(
            config, &fields,
            appliance_route_config_status_string(config->last_status));
        return config->last_status;
    }
    remove = fields.action_set &&
             route_config_value_is(fields.action, fields.action_length,
                                   "remove");
    if (fields.action_set && !remove &&
        !route_config_value_is(fields.action, fields.action_length, "add")) {
        config->last_status = APPLIANCE_ROUTE_CONFIG_BAD_ACTION;
        route_config_record_refusal(
            config, &fields,
            appliance_route_config_status_string(config->last_status));
        return config->last_status;
    }

    /*
     * The candidate starts as a copy of the applied table and one validated
     * operation is applied to it.  The model mutates nothing on a refusal, so
     * the candidate is still the applied table when the refusal returns.
     */
    config->operation_set = false;
    config->candidate = config->applied;
    if (remove) {
        if (fields.backend_set || fields.port_set || fields.flag_set) {
            config->last_status = APPLIANCE_ROUTE_CONFIG_MALFORMED;
            route_config_record_refusal(
                config, &fields,
                appliance_route_config_status_string(config->last_status));
            return config->last_status;
        }
        /* Remember the route as the applied table carries it, so a cancel or a
         * commit can name the whole route instead of the request text. */
        if (appliance_route_table_lookup(&config->candidate, fields.name,
                                         fields.name_length,
                                         &config->operation) ==
            APPLIANCE_ROUTE_OK) {
            config->operation_set = true;
        }
        route_status = appliance_route_table_remove(
            &config->candidate, fields.name, fields.name_length);
    } else {
        bool tls_to_backend = false;

        if (!fields.backend_set || !fields.port_set) {
            config->last_status = APPLIANCE_ROUTE_CONFIG_MISSING_FIELD;
            route_config_record_refusal(
                config, &fields,
                appliance_route_config_status_string(config->last_status));
            return config->last_status;
        }
        if (fields.flag_set) {
            if (route_config_value_is(fields.flag, fields.flag_length, "on")) {
                tls_to_backend = true;
            } else if (!route_config_value_is(fields.flag,
                                              fields.flag_length, "off")) {
                config->last_status = APPLIANCE_ROUTE_CONFIG_BAD_FLAG;
                route_config_record_refusal(
                    config, &fields,
                    appliance_route_config_status_string(config->last_status));
                return config->last_status;
            }
        }
        route_status = appliance_route_table_add(
            &config->candidate, fields.name, fields.name_length,
            fields.backend, fields.backend_length,
            fields.port, fields.port_length, tls_to_backend,
            APPLIANCE_ROUTE_SOURCE_PANEL);
        if (route_status == APPLIANCE_ROUTE_OK) {
            /* The model normalized the name; record that value, not the input. */
            if (appliance_route_table_lookup(&config->candidate, fields.name,
                                             fields.name_length,
                                             &config->operation) ==
                APPLIANCE_ROUTE_OK) {
                config->operation_set = true;
            }
        }
    }
    config->last_route_status = route_status;
    if (route_status != APPLIANCE_ROUTE_OK) {
        config->staged = false;
        config->operation_set = false;
        ++config->refused_count;
        config->last_status = APPLIANCE_ROUTE_CONFIG_REFUSED;
        route_config_record_refusal(
            config, &fields,
            appliance_route_status_string(route_status));
        return config->last_status;
    }
    config->staged = true;
    ++config->stage_count;
    config->last_status = APPLIANCE_ROUTE_CONFIG_OK;
    return APPLIANCE_ROUTE_CONFIG_OK;
}

enum appliance_route_config_status appliance_route_config_commit(
    struct appliance_route_config *config)
{
    if (config == NULL) {
        return APPLIANCE_ROUTE_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->version != APPLIANCE_ROUTE_CONFIG_VERSION) {
        return APPLIANCE_ROUTE_CONFIG_BAD_VERSION;
    }
    if (!config->staged) {
        config->last_status = APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE;
        return config->last_status;
    }
    config->applied = config->candidate;
    config->staged = false;
    ++config->commit_count;
    config->last_status = APPLIANCE_ROUTE_CONFIG_OK;
    route_config_record_operation(config, APPLIANCE_ROUTE_CONFIG_RECORD_APPLIED,
                                  "APPLIED");
    return APPLIANCE_ROUTE_CONFIG_OK;
}

enum appliance_route_config_status appliance_route_config_rollback(
    struct appliance_route_config *config)
{
    if (config == NULL) {
        return APPLIANCE_ROUTE_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->version != APPLIANCE_ROUTE_CONFIG_VERSION) {
        return APPLIANCE_ROUTE_CONFIG_BAD_VERSION;
    }
    if (!config->staged) {
        config->last_status = APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE;
        return config->last_status;
    }
    config->candidate = config->applied;
    config->staged = false;
    ++config->rollback_count;
    config->last_status = APPLIANCE_ROUTE_CONFIG_OK;
    route_config_record_operation(
        config, APPLIANCE_ROUTE_CONFIG_RECORD_CANCELLED, "CANCELLED");
    return APPLIANCE_ROUTE_CONFIG_OK;
}

const struct appliance_route_table *appliance_route_config_applied(
    const struct appliance_route_config *config)
{
    if (config == NULL || !config->initialized) {
        return NULL;
    }
    return &config->applied;
}

const struct appliance_route_table *appliance_route_config_candidate(
    const struct appliance_route_config *config)
{
    if (config == NULL || !config->initialized || !config->staged) {
        return NULL;
    }
    return &config->candidate;
}

enum appliance_route_config_status appliance_route_config_restore(
    struct appliance_route_config *config,
    const struct appliance_route_table *table)
{
    enum appliance_route_status route_status;

    if (config == NULL) {
        return APPLIANCE_ROUTE_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->version != APPLIANCE_ROUTE_CONFIG_VERSION) {
        return APPLIANCE_ROUTE_CONFIG_BAD_VERSION;
    }
    if (table == NULL) {
        config->last_status = APPLIANCE_ROUTE_CONFIG_NULL;
        return config->last_status;
    }
    route_status = appliance_route_table_validate(table);
    config->last_route_status = route_status;
    if (route_status != APPLIANCE_ROUTE_OK) {
        config->last_status = APPLIANCE_ROUTE_CONFIG_REFUSED;
        return config->last_status;
    }
    /*
     * A stored table arrives whole or not at all: the candidate is dropped and
     * the applied table is replaced in one assignment, exactly as the commit
     * does for a table the panel built.  No record is produced here: restoring
     * is the boot reading the machine's own state, not an operation an
     * operator performed.
     */
    config->applied = *table;
    config->candidate = *table;
    config->staged = false;
    config->operation_set = false;
    config->last_status = APPLIANCE_ROUTE_CONFIG_OK;
    return APPLIANCE_ROUTE_CONFIG_OK;
}

bool appliance_route_config_staged(const struct appliance_route_config *config)
{
    return config != NULL && config->initialized && config->staged;
}

enum appliance_route_config_status appliance_route_config_last_status(
    const struct appliance_route_config *config)
{
    if (config == NULL) {
        return APPLIANCE_ROUTE_CONFIG_NULL;
    }
    return config->last_status;
}

enum appliance_route_status appliance_route_config_last_route_status(
    const struct appliance_route_config *config)
{
    if (config == NULL) {
        return APPLIANCE_ROUTE_BAD_VERSION;
    }
    return config->last_route_status;
}

uint32_t appliance_route_config_stage_count(
    const struct appliance_route_config *config)
{
    return config != NULL ? config->stage_count : 0U;
}

uint32_t appliance_route_config_commit_count(
    const struct appliance_route_config *config)
{
    return config != NULL ? config->commit_count : 0U;
}

uint32_t appliance_route_config_rollback_count(
    const struct appliance_route_config *config)
{
    return config != NULL ? config->rollback_count : 0U;
}

uint32_t appliance_route_config_refused_count(
    const struct appliance_route_config *config)
{
    return config != NULL ? config->refused_count : 0U;
}

bool appliance_route_config_take_record(
    struct appliance_route_config *config,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    if (config == NULL || buffer == NULL || length_out == NULL ||
        !config->initialized ||
        config->version != APPLIANCE_ROUTE_CONFIG_VERSION) {
        return false;
    }
    if (!config->record_pending) {
        return false;
    }
    if (config->record_length + 1U > capacity) {
        /* Hand nothing over and drop nothing: a later, larger buffer gets it. */
        return false;
    }
    memcpy(buffer, config->record, config->record_length);
    buffer[config->record_length] = '\0';
    *length_out = config->record_length;
    config->record_pending = false;
    config->record_length = 0U;
    config->record[0] = '\0';
    return true;
}

enum appliance_route_config_record_result appliance_route_config_record_result(
    const struct appliance_route_config *config)
{
    if (config == NULL || !config->initialized) {
        return APPLIANCE_ROUTE_CONFIG_RECORD_NONE;
    }
    return config->record_result;
}

uint32_t appliance_route_config_record_count(
    const struct appliance_route_config *config)
{
    return config != NULL ? config->record_count : 0U;
}

const char *appliance_route_config_record_result_string(
    enum appliance_route_config_record_result result)
{
    switch (result) {
    case APPLIANCE_ROUTE_CONFIG_RECORD_NONE:
        return "NONE";
    case APPLIANCE_ROUTE_CONFIG_RECORD_APPLIED:
        return "APPLIED";
    case APPLIANCE_ROUTE_CONFIG_RECORD_REFUSED:
        return "REFUSED";
    case APPLIANCE_ROUTE_CONFIG_RECORD_CANCELLED:
        return "CANCELLED";
    }
    return "UNKNOWN";
}

const char *appliance_route_config_status_string(
    enum appliance_route_config_status status)
{
    switch (status) {
    case APPLIANCE_ROUTE_CONFIG_OK:
        return "OK";
    case APPLIANCE_ROUTE_CONFIG_NULL:
        return "NULL";
    case APPLIANCE_ROUTE_CONFIG_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_ROUTE_CONFIG_BAD_CONTENT_TYPE:
        return "BAD_CONTENT_TYPE";
    case APPLIANCE_ROUTE_CONFIG_MALFORMED:
        return "MALFORMED";
    case APPLIANCE_ROUTE_CONFIG_MISSING_FIELD:
        return "MISSING_FIELD";
    case APPLIANCE_ROUTE_CONFIG_BAD_ACTION:
        return "BAD_ACTION";
    case APPLIANCE_ROUTE_CONFIG_BAD_FLAG:
        return "BAD_FLAG";
    case APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE:
        return "NO_CANDIDATE";
    case APPLIANCE_ROUTE_CONFIG_REFUSED:
        return "REFUSED";
    }
    return "UNKNOWN";
}
