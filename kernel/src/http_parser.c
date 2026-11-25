#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HTTP_PARSER_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static bool http_parser_generation_valid(uint32_t generation)
{
    return generation >= HTTP_PARSER_GENERATION_MIN &&
           generation <= HTTP_PARSER_GENERATION_MAX;
}

static bool http_parser_progress_zero(
    const struct http_parser_progress *progress)
{
    return progress->request_line_offset == 0U &&
           progress->request_line_count == 0U &&
           progress->header_offset == 0U &&
           progress->header_count == 0U &&
           progress->total_count == 0U;
}

static bool http_parser_progress_shape_valid(
    const struct http_parser_progress *progress,
    enum http_parser_state state)
{
    if (progress == NULL ||
        progress->request_line_offset != 0U ||
        progress->request_line_count > HTTP_PARSER_REQUEST_LINE_MAX ||
        progress->header_count > HTTP_PARSER_HEADERS_MAX) {
        return false;
    }

    switch (state) {
    case HTTP_PARSER_STATE_EMPTY:
        return http_parser_progress_zero(progress);
    case HTTP_PARSER_STATE_REQUEST_LINE:
        return progress->request_line_count > 0U &&
               progress->header_offset == 0U &&
               progress->header_count == 0U &&
               progress->total_count == progress->request_line_count;
    case HTTP_PARSER_STATE_HEADERS:
        return progress->request_line_count > 0U &&
               progress->header_offset == progress->request_line_count &&
               progress->total_count ==
                   progress->request_line_count +
                   progress->header_count;
    case HTTP_PARSER_STATE_COMPLETE:
        return progress->request_line_count > 0U &&
               progress->header_offset == progress->request_line_count &&
               progress->header_count > 0U &&
               progress->total_count ==
                   progress->request_line_count +
                   progress->header_count;
    case HTTP_PARSER_STATE_REJECTED:
        if (progress->request_line_count == 0U) {
            return progress->header_offset == 0U &&
                   progress->header_count == 0U &&
                   progress->total_count == 0U;
        }
        if (progress->header_offset == 0U) {
            return progress->header_count == 0U &&
                   progress->total_count ==
                       progress->request_line_count;
        }
        return progress->header_offset ==
                   progress->request_line_count &&
               progress->total_count ==
                   progress->request_line_count +
                   progress->header_count;
    default:
        return false;
    }
}

static bool http_parser_entry_valid(
    const struct http_parser_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (entry->request_line_stored > HTTP_PARSER_REQUEST_LINE_MAX ||
        (entry->request_line_complete &&
         entry->request_line_fields.line_length !=
             entry->request_line_stored)) {
        return false;
    }
    if (!entry->active) {
        return entry->generation == 0U &&
               entry->state == HTTP_PARSER_STATE_EMPTY &&
               http_parser_progress_zero(&entry->progress) &&
               entry->request_line_stored == 0U &&
               entry->request_line_stage == 0U &&
               !entry->request_line_complete &&
               entry->request_line_error == HTTP_PARSER_OK &&
               entry->request_line_fields.method_offset == 0U &&
               entry->request_line_fields.method_length == 0U &&
               entry->request_line_fields.target_offset == 0U &&
               entry->request_line_fields.target_length == 0U &&
               entry->request_line_fields.version_offset == 0U &&
               entry->request_line_fields.version_length == 0U &&
               entry->request_line_fields.line_length == 0U &&
               entry->header_stored == 0U &&
               entry->header_stage == 0U &&
               !entry->header_complete &&
               entry->header_fields == 0U &&
               entry->header_error == HTTP_PARSER_OK;
    }
    if (!http_parser_generation_valid(entry->generation)) {
        return false;
    }
    if (entry->state == HTTP_PARSER_STATE_EMPTY &&
        (entry->request_line_stored != 0U ||
         entry->request_line_complete)) {
        return false;
    }
    if (entry->request_line_complete &&
        entry->request_line_error == HTTP_PARSER_OK &&
        (entry->request_line_stored == 0U ||
         entry->progress.request_line_count !=
             entry->request_line_stored ||
         entry->request_line_fields.method_length == 0U ||
         entry->request_line_fields.target_length == 0U ||
         entry->request_line_fields.version_length == 0U)) {
        return false;
    }
    if (entry->header_stored > HTTP_PARSER_HEADERS_MAX) {
        return false;
    }
    if (entry->header_complete &&
        (entry->state != HTTP_PARSER_STATE_COMPLETE ||
         entry->header_error != HTTP_PARSER_OK ||
         entry->header_stored == 0U ||
         entry->progress.header_count != entry->header_stored ||
         entry->progress.header_offset != entry->request_line_stored ||
         entry->progress.total_count !=
             entry->request_line_stored +
             entry->header_stored)) {
        return false;
    }
    if (entry->state == HTTP_PARSER_STATE_COMPLETE &&
        entry->header_error != HTTP_PARSER_OK) {
        return false;
    }
    if ((entry->state == HTTP_PARSER_STATE_EMPTY ||
         entry->state == HTTP_PARSER_STATE_REQUEST_LINE) &&
        (entry->header_stored != 0U ||
         entry->header_stage != 0U ||
         entry->header_complete ||
         entry->header_fields != 0U ||
         entry->header_error != HTTP_PARSER_OK)) {
        return false;
    }
    return http_parser_progress_shape_valid(
        &entry->progress, entry->state);
}

static bool http_parser_valid(
    const struct http_parser *parser)
{
    uint32_t active_count = 0U;
    uint32_t index;

    if (parser == NULL ||
        !parser->published ||
        parser->version != HTTP_PARSER_VERSION ||
        parser->active_count > HTTP_PARSER_SLOT_COUNT) {
        return false;
    }
    for (index = 0U; index < HTTP_PARSER_SLOT_COUNT; ++index) {
        const struct http_parser_entry *entry =
            &parser->entries[index];

        if (!http_parser_entry_valid(entry)) {
            return false;
        }
        if (entry->active) {
            ++active_count;
        }
    }
    return active_count == parser->active_count;
}

static enum http_parser_status http_parser_validate_token(
    struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_parser_entry **entry_out)
{
    struct http_parser_entry *entry;

    if (parser == NULL || token == NULL || entry_out == NULL) {
        return HTTP_PARSER_NULL;
    }
    if (!http_parser_valid(parser)) {
        return HTTP_PARSER_BAD_STATE;
    }
    if (token->index >= HTTP_PARSER_SLOT_COUNT ||
        !http_parser_generation_valid(token->generation)) {
        return HTTP_PARSER_BAD_TOKEN;
    }
    entry = &parser->entries[token->index];
    if (!entry->active) {
        return HTTP_PARSER_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HTTP_PARSER_STALE_TOKEN;
    }
    *entry_out = entry;
    return HTTP_PARSER_OK;
}

static bool http_parser_progress_monotonic(
    const struct http_parser_progress *current,
    const struct http_parser_progress *next)
{
    return next->request_line_count >= current->request_line_count &&
           next->header_count >= current->header_count &&
           next->total_count >= current->total_count;
}

static enum http_parser_status http_parser_advance_validate(
    const struct http_parser_entry *entry,
    enum http_parser_state next_state,
    const struct http_parser_progress *progress)
{
    const struct http_parser_progress *current = &entry->progress;

    if (entry->state == HTTP_PARSER_STATE_EMPTY) {
        if (next_state != HTTP_PARSER_STATE_REQUEST_LINE) {
            return HTTP_PARSER_INVALID_PREDECESSOR;
        }
        if (!http_parser_progress_shape_valid(
                progress, next_state)) {
            return HTTP_PARSER_INVALID_PROGRESS;
        }
        return HTTP_PARSER_OK;
    }

    if (entry->state == HTTP_PARSER_STATE_REQUEST_LINE) {
        if (next_state == HTTP_PARSER_STATE_REQUEST_LINE) {
            if (!http_parser_progress_shape_valid(
                    progress, next_state) ||
                !http_parser_progress_monotonic(
                    current, progress) ||
                progress->request_line_count <=
                    current->request_line_count) {
                return HTTP_PARSER_INVALID_PROGRESS;
            }
            return HTTP_PARSER_OK;
        }
        if (next_state == HTTP_PARSER_STATE_HEADERS) {
            if (!http_parser_progress_shape_valid(
                    progress, next_state) ||
                !http_parser_progress_monotonic(
                    current, progress) ||
                progress->header_count != 0U) {
                return HTTP_PARSER_INVALID_PROGRESS;
            }
            return HTTP_PARSER_OK;
        }
        return HTTP_PARSER_INVALID_PREDECESSOR;
    }

    if (entry->state == HTTP_PARSER_STATE_HEADERS) {
        if (next_state == HTTP_PARSER_STATE_HEADERS) {
            if (!http_parser_progress_shape_valid(
                    progress, next_state) ||
                !http_parser_progress_monotonic(
                    current, progress) ||
                progress->request_line_count !=
                    current->request_line_count ||
                progress->header_count <=
                    current->header_count) {
                return HTTP_PARSER_INVALID_PROGRESS;
            }
            return HTTP_PARSER_OK;
        }
        if (next_state == HTTP_PARSER_STATE_COMPLETE) {
            if (!http_parser_progress_shape_valid(
                    progress, next_state) ||
                !http_parser_progress_monotonic(
                    current, progress) ||
                progress->request_line_count !=
                    current->request_line_count ||
                progress->header_count <
                    current->header_count) {
                return HTTP_PARSER_INVALID_PROGRESS;
            }
            return HTTP_PARSER_OK;
        }
        return HTTP_PARSER_INVALID_PREDECESSOR;
    }

    return HTTP_PARSER_TERMINAL;
}

static enum http_parser_status http_parser_reject_validate(
    const struct http_parser_entry *entry,
    const struct http_parser_progress *progress)
{
    const struct http_parser_progress *current = &entry->progress;

    if (!http_parser_progress_shape_valid(
            progress, HTTP_PARSER_STATE_REJECTED) ||
        !http_parser_progress_monotonic(current, progress)) {
        return HTTP_PARSER_INVALID_PROGRESS;
    }
    if (entry->state == HTTP_PARSER_STATE_EMPTY) {
        return http_parser_progress_zero(progress)
            ? HTTP_PARSER_OK
            : HTTP_PARSER_INVALID_PROGRESS;
    }
    if (entry->state == HTTP_PARSER_STATE_REQUEST_LINE) {
        return progress->request_line_count >=
                   current->request_line_count
            ? HTTP_PARSER_OK
            : HTTP_PARSER_INVALID_PROGRESS;
    }
    if (entry->state == HTTP_PARSER_STATE_HEADERS) {
        return progress->request_line_count ==
                   current->request_line_count &&
               progress->header_count >= current->header_count
            ? HTTP_PARSER_OK
            : HTTP_PARSER_INVALID_PROGRESS;
    }
    return HTTP_PARSER_TERMINAL;
}

enum http_parser_status http_parser_init(
    struct http_parser *parser)
{
    if (parser == NULL) {
        return HTTP_PARSER_NULL;
    }
    if (parser->published) {
        return HTTP_PARSER_BAD_STATE;
    }
    memset(parser, 0, sizeof(*parser));
    parser->version = HTTP_PARSER_VERSION;
    parser->published = true;
    return HTTP_PARSER_OK;
}

enum http_parser_status http_parser_start(
    struct http_parser *parser,
    const struct http_parser_token *token)
{
    struct http_parser_entry *entry;

    if (parser == NULL || token == NULL) {
        return HTTP_PARSER_NULL;
    }
    if (!http_parser_valid(parser)) {
        return HTTP_PARSER_BAD_STATE;
    }
    if (token->index >= HTTP_PARSER_SLOT_COUNT ||
        !http_parser_generation_valid(token->generation)) {
        return HTTP_PARSER_BAD_TOKEN;
    }

    entry = &parser->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HTTP_PARSER_ALREADY_ATTACHED
            : HTTP_PARSER_STALE_TOKEN;
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->state = HTTP_PARSER_STATE_EMPTY;
    ++parser->active_count;
    return HTTP_PARSER_OK;
}

enum http_parser_status http_parser_advance(
    struct http_parser *parser,
    const struct http_parser_token *token,
    enum http_parser_state next_state,
    const struct http_parser_progress *progress)
{
    struct http_parser_entry *entry;
    enum http_parser_status token_status;
    enum http_parser_status validate_status;

    if (parser == NULL || token == NULL || progress == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_parser_validate_token(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (entry->request_line_error != HTTP_PARSER_OK) {
        return entry->request_line_error;
    }
    if (entry->header_error != HTTP_PARSER_OK) {
        return entry->header_error;
    }
    if (progress->request_line_count >
            HTTP_PARSER_REQUEST_LINE_MAX) {
        return HTTP_PARSER_REQUEST_LINE_LIMIT;
    }
    if (progress->header_count > HTTP_PARSER_HEADERS_MAX) {
        return HTTP_PARSER_HEADERS_LIMIT;
    }
    validate_status = http_parser_advance_validate(
        entry, next_state, progress);
    if (validate_status != HTTP_PARSER_OK) {
        return validate_status;
    }
    if (entry->state == HTTP_PARSER_STATE_REQUEST_LINE &&
        next_state == HTTP_PARSER_STATE_HEADERS) {
        entry->header_stored = 0U;
        entry->header_stage = 0U;
        entry->header_complete = false;
        entry->header_fields = 0U;
        entry->header_error = HTTP_PARSER_OK;
    }
    entry->state = next_state;
    entry->progress = *progress;
    return HTTP_PARSER_OK;
}

enum http_parser_status http_parser_reject(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const struct http_parser_progress *progress)
{
    struct http_parser_entry *entry;
    enum http_parser_status token_status;
    enum http_parser_status validate_status;

    if (parser == NULL || token == NULL || progress == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_parser_validate_token(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (progress->request_line_count >
            HTTP_PARSER_REQUEST_LINE_MAX) {
        return HTTP_PARSER_REQUEST_LINE_LIMIT;
    }
    if (progress->header_count > HTTP_PARSER_HEADERS_MAX) {
        return HTTP_PARSER_HEADERS_LIMIT;
    }
    validate_status = http_parser_reject_validate(
        entry, progress);
    if (validate_status != HTTP_PARSER_OK) {
        return validate_status;
    }
    entry->state = HTTP_PARSER_STATE_REJECTED;
    entry->progress = *progress;
    return HTTP_PARSER_OK;
}

enum http_parser_status http_parser_release(
    struct http_parser *parser,
    const struct http_parser_token *token)
{
    struct http_parser_entry *entry;
    enum http_parser_status token_status;

    if (parser == NULL || token == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_parser_validate_token(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    memset(entry, 0, sizeof(*entry));
    entry->state = HTTP_PARSER_STATE_EMPTY;
    --parser->active_count;
    return HTTP_PARSER_OK;
}

bool http_parser_published(
    const struct http_parser *parser)
{
    return http_parser_valid(parser);
}

uint32_t http_parser_active_count(
    const struct http_parser *parser)
{
    return http_parser_valid(parser)
        ? parser->active_count
        : 0U;
}

uint32_t http_parser_request_line_max(
    const struct http_parser *parser)
{
    return http_parser_valid(parser)
        ? HTTP_PARSER_REQUEST_LINE_MAX
        : 0U;
}

uint32_t http_parser_headers_max(
    const struct http_parser *parser)
{
    return http_parser_valid(parser)
        ? HTTP_PARSER_HEADERS_MAX
        : 0U;
}

enum http_parser_state http_parser_state_at(
    const struct http_parser *parser,
    uint32_t index)
{
    if (!http_parser_valid(parser) ||
        index >= HTTP_PARSER_SLOT_COUNT) {
        return HTTP_PARSER_STATE_EMPTY;
    }
    return parser->entries[index].state;
}

uint32_t http_parser_generation_at(
    const struct http_parser *parser,
    uint32_t index)
{
    if (!http_parser_valid(parser) ||
        index >= HTTP_PARSER_SLOT_COUNT) {
        return 0U;
    }
    return parser->entries[index].generation;
}

enum http_parser_status http_parser_progress_at(
    const struct http_parser *parser,
    uint32_t index,
    struct http_parser_progress *progress)
{
    if (parser == NULL || progress == NULL) {
        return HTTP_PARSER_NULL;
    }
    if (!http_parser_valid(parser)) {
        return HTTP_PARSER_BAD_STATE;
    }
    if (index >= HTTP_PARSER_SLOT_COUNT) {
        return HTTP_PARSER_BAD_TOKEN;
    }
    *progress = parser->entries[index].progress;
    return HTTP_PARSER_OK;
}

enum http_parser_state http_parser_state(
    const struct http_parser *parser,
    const struct http_parser_token *token)
{
    if (!http_parser_valid(parser) ||
        token == NULL ||
        token->index >= HTTP_PARSER_SLOT_COUNT ||
        !http_parser_generation_valid(token->generation) ||
        !parser->entries[token->index].active ||
        parser->entries[token->index].generation !=
            token->generation) {
        return HTTP_PARSER_STATE_EMPTY;
    }
    return parser->entries[token->index].state;
}

uint32_t http_parser_generation(
    const struct http_parser *parser,
    const struct http_parser_token *token)
{
    if (!http_parser_valid(parser) ||
        token == NULL ||
        token->index >= HTTP_PARSER_SLOT_COUNT ||
        !http_parser_generation_valid(token->generation) ||
        !parser->entries[token->index].active ||
        parser->entries[token->index].generation !=
            token->generation) {
        return 0U;
    }
    return parser->entries[token->index].generation;
}

enum http_parser_status http_parser_progress(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_parser_progress *progress)
{
    if (parser == NULL || token == NULL || progress == NULL) {
        return HTTP_PARSER_NULL;
    }
    if (!http_parser_valid(parser)) {
        return HTTP_PARSER_BAD_STATE;
    }
    if (token->index >= HTTP_PARSER_SLOT_COUNT ||
        !http_parser_generation_valid(token->generation)) {
        return HTTP_PARSER_BAD_TOKEN;
    }
    if (!parser->entries[token->index].active) {
        return HTTP_PARSER_NOT_ATTACHED;
    }
    if (parser->entries[token->index].generation !=
        token->generation) {
        return HTTP_PARSER_STALE_TOKEN;
    }
    *progress = parser->entries[token->index].progress;
    return HTTP_PARSER_OK;
}

const char *http_parser_state_string(
    enum http_parser_state state)
{
    switch (state) {
    case HTTP_PARSER_STATE_EMPTY:
        return "HTTP_PARSER_STATE_EMPTY";
    case HTTP_PARSER_STATE_REQUEST_LINE:
        return "HTTP_PARSER_STATE_REQUEST_LINE";
    case HTTP_PARSER_STATE_HEADERS:
        return "HTTP_PARSER_STATE_HEADERS";
    case HTTP_PARSER_STATE_COMPLETE:
        return "HTTP_PARSER_STATE_COMPLETE";
    case HTTP_PARSER_STATE_REJECTED:
        return "HTTP_PARSER_STATE_REJECTED";
    default:
        return NULL;
    }
}

const char *http_parser_status_string(
    enum http_parser_status status)
{
    switch (status) {
    case HTTP_PARSER_OK:
        return "HTTP_PARSER_OK";
    case HTTP_PARSER_NULL:
        return "HTTP_PARSER_NULL";
    case HTTP_PARSER_BAD_STATE:
        return "HTTP_PARSER_BAD_STATE";
    case HTTP_PARSER_BAD_TOKEN:
        return "HTTP_PARSER_BAD_TOKEN";
    case HTTP_PARSER_STALE_TOKEN:
        return "HTTP_PARSER_STALE_TOKEN";
    case HTTP_PARSER_NOT_ATTACHED:
        return "HTTP_PARSER_NOT_ATTACHED";
    case HTTP_PARSER_ALREADY_ATTACHED:
        return "HTTP_PARSER_ALREADY_ATTACHED";
    case HTTP_PARSER_INVALID_PREDECESSOR:
        return "HTTP_PARSER_INVALID_PREDECESSOR";
    case HTTP_PARSER_INVALID_PROGRESS:
        return "HTTP_PARSER_INVALID_PROGRESS";
    case HTTP_PARSER_REQUEST_LINE_LIMIT:
        return "HTTP_PARSER_REQUEST_LINE_LIMIT";
    case HTTP_PARSER_HEADERS_LIMIT:
        return "HTTP_PARSER_HEADERS_LIMIT";
    case HTTP_PARSER_TERMINAL:
        return "HTTP_PARSER_TERMINAL";
    case HTTP_PARSER_REQUEST_LINE_INCOMPLETE:
        return "HTTP_PARSER_REQUEST_LINE_INCOMPLETE";
    case HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER:
        return "HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER";
    case HTTP_PARSER_REQUEST_LINE_INVALID_SPACING:
        return "HTTP_PARSER_REQUEST_LINE_INVALID_SPACING";
    case HTTP_PARSER_REQUEST_LINE_INVALID_TARGET:
        return "HTTP_PARSER_REQUEST_LINE_INVALID_TARGET";
    case HTTP_PARSER_REQUEST_LINE_INVALID_VERSION:
        return "HTTP_PARSER_REQUEST_LINE_INVALID_VERSION";
    case HTTP_PARSER_HEADER_INCOMPLETE:
        return "HTTP_PARSER_HEADER_INCOMPLETE";
    case HTTP_PARSER_HEADER_INVALID_NAME:
        return "HTTP_PARSER_HEADER_INVALID_NAME";
    case HTTP_PARSER_HEADER_INVALID_VALUE:
        return "HTTP_PARSER_HEADER_INVALID_VALUE";
    case HTTP_PARSER_HEADER_FOLDING:
        return "HTTP_PARSER_HEADER_FOLDING";
    case HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION:
        return "HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION";
    case HTTP_PARSER_HEADER_OUT_OF_RANGE:
        return "HTTP_PARSER_HEADER_OUT_OF_RANGE";
    default:
        return NULL;
    }
}

#if defined(HTTP_REQUEST_LINE_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static bool http_request_line_tchar(uint8_t value)
{
    if ((value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z')) {
        return true;
    }
    switch (value) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

static bool http_request_line_digit(uint8_t value)
{
    return value >= '0' && value <= '9';
}

static bool http_request_line_version_valid(
    const uint8_t *data,
    uint32_t offset,
    uint32_t length)
{
    return length == 8U &&
           memcmp(data + offset, "HTTP/", 5U) == 0 &&
           http_request_line_digit(data[offset + 5U]) &&
           data[offset + 6U] == '.' &&
           http_request_line_digit(data[offset + 7U]);
}

static enum http_parser_status http_request_line_fail(
    struct http_parser_entry *entry,
    enum http_parser_status status)
{
    entry->request_line_error = status;
    return status;
}

static void http_request_line_publish_progress(
    struct http_parser_entry *entry)
{
    entry->state = HTTP_PARSER_STATE_REQUEST_LINE;
    memset(&entry->progress, 0, sizeof(entry->progress));
    entry->progress.request_line_count = entry->request_line_stored;
    entry->progress.total_count = entry->request_line_stored;
}

enum http_parser_status http_request_line_feed(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const uint8_t *bytes,
    uint32_t length,
    bool *complete)
{
    struct http_parser_entry *entry;
    enum http_parser_status token_status;
    enum http_parser_status error = HTTP_PARSER_OK;
    uint32_t previous_stored;
    uint32_t index;

    if (parser == NULL ||
        token == NULL ||
        bytes == NULL ||
        complete == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_parser_validate_token(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (entry->state != HTTP_PARSER_STATE_EMPTY &&
        entry->state != HTTP_PARSER_STATE_REQUEST_LINE) {
        return HTTP_PARSER_TERMINAL;
    }
    if (entry->request_line_error != HTTP_PARSER_OK) {
        return entry->request_line_error;
    }
    if (entry->request_line_complete) {
        return HTTP_PARSER_TERMINAL;
    }

    *complete = false;
    previous_stored = entry->request_line_stored;

    for (index = 0U; index < length; ++index) {
        uint8_t value = bytes[index];

        if (entry->request_line_stored >=
            HTTP_PARSER_REQUEST_LINE_MAX) {
            error = HTTP_PARSER_REQUEST_LINE_LIMIT;
            break;
        }
        if (entry->request_line_stage == 3U) {
            if (value != '\n') {
                error = HTTP_PARSER_REQUEST_LINE_INVALID_SPACING;
                break;
            }
            entry->request_line_data[
                entry->request_line_stored++] = value;
            entry->request_line_stage = 4U;
            entry->request_line_complete = true;
            entry->request_line_fields.line_length =
                entry->request_line_stored;
            break;
        }
        if (value == '\r') {
            if (entry->request_line_fields.method_length == 0U ||
                entry->request_line_fields.target_length == 0U ||
                entry->request_line_fields.version_length == 0U) {
                error = HTTP_PARSER_REQUEST_LINE_INVALID_SPACING;
                break;
            }
            if (entry->request_line_stage == 2U &&
                !http_request_line_version_valid(
                    entry->request_line_data,
                    entry->request_line_fields.version_offset,
                    entry->request_line_fields.version_length)) {
                error = HTTP_PARSER_REQUEST_LINE_INVALID_VERSION;
                break;
            }
            entry->request_line_data[
                entry->request_line_stored++] = value;
            entry->request_line_stage = 3U;
            continue;
        }
        if (value == '\n') {
            error = HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER;
            break;
        }
        if (value == ' ') {
            if (entry->request_line_stage == 0U) {
                if (entry->request_line_fields.method_length == 0U) {
                    error = HTTP_PARSER_REQUEST_LINE_INVALID_SPACING;
                    break;
                }
                entry->request_line_fields.target_offset =
                    entry->request_line_fields.method_length + 1U;
                entry->request_line_stage = 1U;
                entry->request_line_data[
                    entry->request_line_stored++] = value;
                continue;
            }
            if (entry->request_line_stage == 1U) {
                if (entry->request_line_fields.target_length == 0U) {
                    error = HTTP_PARSER_REQUEST_LINE_INVALID_SPACING;
                    break;
                }
                entry->request_line_fields.version_offset =
                    entry->request_line_fields.method_length +
                    1U +
                    entry->request_line_fields.target_length +
                    1U;
                entry->request_line_stage = 2U;
                entry->request_line_data[
                    entry->request_line_stored++] = value;
                continue;
            }
            error = HTTP_PARSER_REQUEST_LINE_INVALID_SPACING;
            break;
        }
        if (value < 0x21U || value == 0x7FU) {
            error = HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER;
            break;
        }
        if (entry->request_line_stage == 1U) {
            if (entry->request_line_fields.target_length == 0U &&
                value != '/') {
                error = HTTP_PARSER_REQUEST_LINE_INVALID_TARGET;
                break;
            }
            if (value == '#' || value > 0x7EU) {
                error = HTTP_PARSER_REQUEST_LINE_INVALID_TARGET;
                break;
            }
        }
        if (entry->request_line_stage == 0U &&
            !http_request_line_tchar(value)) {
            error = HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER;
            break;
        }

        entry->request_line_data[entry->request_line_stored++] = value;
        if (entry->request_line_stage == 0U) {
            ++entry->request_line_fields.method_length;
        } else if (entry->request_line_stage == 1U) {
            ++entry->request_line_fields.target_length;
        } else {
            ++entry->request_line_fields.version_length;
        }
    }

    if (entry->request_line_stored != previous_stored) {
        http_request_line_publish_progress(entry);
    }
    if (error != HTTP_PARSER_OK) {
        return http_request_line_fail(entry, error);
    }
    *complete = entry->request_line_complete;
    return HTTP_PARSER_OK;
}

static enum http_parser_status http_request_line_validate_const(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    const struct http_parser_entry **entry_out)
{
    const struct http_parser_entry *entry;

    if (parser == NULL || token == NULL || entry_out == NULL) {
        return HTTP_PARSER_NULL;
    }
    if (!http_parser_valid(parser)) {
        return HTTP_PARSER_BAD_STATE;
    }
    if (token->index >= HTTP_PARSER_SLOT_COUNT ||
        !http_parser_generation_valid(token->generation)) {
        return HTTP_PARSER_BAD_TOKEN;
    }
    entry = &parser->entries[token->index];
    if (!entry->active) {
        return HTTP_PARSER_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HTTP_PARSER_STALE_TOKEN;
    }
    *entry_out = entry;
    return HTTP_PARSER_OK;
}

enum http_parser_status http_request_line_get(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_request_line *line)
{
    const struct http_parser_entry *entry;
    enum http_parser_status token_status;

    if (parser == NULL || token == NULL || line == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_request_line_validate_const(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (entry->request_line_error != HTTP_PARSER_OK) {
        return entry->request_line_error;
    }
    if (!entry->request_line_complete) {
        return HTTP_PARSER_REQUEST_LINE_INCOMPLETE;
    }

    line->method =
        entry->request_line_data +
        entry->request_line_fields.method_offset;
    line->method_length = entry->request_line_fields.method_length;
    line->target =
        entry->request_line_data +
        entry->request_line_fields.target_offset;
    line->target_length = entry->request_line_fields.target_length;
    line->version =
        entry->request_line_data +
        entry->request_line_fields.version_offset;
    line->version_length = entry->request_line_fields.version_length;
    line->line_length = entry->request_line_fields.line_length;
    return HTTP_PARSER_OK;
}

enum http_parser_status http_request_line_error(
    const struct http_parser *parser,
    const struct http_parser_token *token)
{
    const struct http_parser_entry *entry;
    enum http_parser_status token_status;

    if (parser == NULL || token == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_request_line_validate_const(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    return entry->request_line_error;
}

#if defined(HTTP_HEADER_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

#define HTTP_HEADER_STAGE_START 0U
#define HTTP_HEADER_STAGE_NAME 1U
#define HTTP_HEADER_STAGE_VALUE 2U
#define HTTP_HEADER_STAGE_FIELD_CR 3U
#define HTTP_HEADER_STAGE_BLANK_CR 4U

static bool http_header_value_byte(uint8_t value)
{
    return value == ' ' ||
           value == '\t' ||
           (value >= 0x21U && value <= 0x7EU);
}

static enum http_parser_status http_header_fail(
    struct http_parser_entry *entry,
    enum http_parser_status status)
{
    entry->header_error = status;
    return status;
}

static void http_header_publish_progress(
    struct http_parser_entry *entry,
    enum http_parser_state state)
{
    entry->state = state;
    entry->progress.header_offset = entry->request_line_stored;
    entry->progress.header_count = entry->header_stored;
    entry->progress.total_count =
        entry->request_line_stored +
        entry->header_stored;
}

enum http_parser_status http_header_feed(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const uint8_t *bytes,
    uint32_t length,
    bool *complete)
{
    struct http_parser_entry *entry;
    enum http_parser_status token_status;
    enum http_parser_status error = HTTP_PARSER_OK;
    uint32_t previous_stored;
    uint32_t index;

    if (parser == NULL ||
        token == NULL ||
        bytes == NULL ||
        complete == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_parser_validate_token(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (entry->request_line_error != HTTP_PARSER_OK) {
        return entry->request_line_error;
    }
    if (!entry->request_line_complete) {
        return HTTP_PARSER_REQUEST_LINE_INCOMPLETE;
    }
    if (entry->header_complete) {
        return HTTP_PARSER_TERMINAL;
    }
    if (entry->state != HTTP_PARSER_STATE_HEADERS) {
        return entry->state == HTTP_PARSER_STATE_COMPLETE ||
               entry->state == HTTP_PARSER_STATE_REJECTED
            ? HTTP_PARSER_TERMINAL
            : HTTP_PARSER_INVALID_PREDECESSOR;
    }
    if (entry->header_error != HTTP_PARSER_OK) {
        return entry->header_error;
    }

    *complete = false;
    previous_stored = entry->header_stored;

    for (index = 0U; index < length; ++index) {
        uint8_t value = bytes[index];

        if (entry->header_stored >= HTTP_PARSER_HEADERS_MAX) {
            error = HTTP_PARSER_HEADERS_LIMIT;
            break;
        }

        if (entry->header_stage == HTTP_HEADER_STAGE_START) {
            if (value == '\r') {
                entry->header_data[
                    entry->header_stored++] = value;
                entry->header_stage = HTTP_HEADER_STAGE_BLANK_CR;
                continue;
            }
            if (value == '\n') {
                error = HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION;
                break;
            }
            if (value == ' ' || value == '\t') {
                error = HTTP_PARSER_HEADER_FOLDING;
                break;
            }
            if (!http_request_line_tchar(value)) {
                error = HTTP_PARSER_HEADER_INVALID_NAME;
                break;
            }
            entry->header_data[entry->header_stored++] = value;
            entry->header_stage = HTTP_HEADER_STAGE_NAME;
            continue;
        }

        if (entry->header_stage == HTTP_HEADER_STAGE_NAME) {
            if (value == ':') {
                entry->header_data[
                    entry->header_stored++] = value;
                entry->header_stage = HTTP_HEADER_STAGE_VALUE;
                continue;
            }
            if (value == '\r') {
                error = HTTP_PARSER_HEADER_INVALID_NAME;
                break;
            }
            if (value == '\n') {
                error = HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION;
                break;
            }
            if (value == ' ' || value == '\t' ||
                !http_request_line_tchar(value)) {
                error = HTTP_PARSER_HEADER_INVALID_NAME;
                break;
            }
            entry->header_data[entry->header_stored++] = value;
            continue;
        }

        if (entry->header_stage == HTTP_HEADER_STAGE_VALUE) {
            if (value == '\r') {
                entry->header_data[
                    entry->header_stored++] = value;
                entry->header_stage = HTTP_HEADER_STAGE_FIELD_CR;
                continue;
            }
            if (value == '\n') {
                error = HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION;
                break;
            }
            if (!http_header_value_byte(value)) {
                error = HTTP_PARSER_HEADER_INVALID_VALUE;
                break;
            }
            entry->header_data[entry->header_stored++] = value;
            continue;
        }

        if (entry->header_stage == HTTP_HEADER_STAGE_FIELD_CR) {
            if (value != '\n') {
                error = HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION;
                break;
            }
            entry->header_data[entry->header_stored++] = value;
            ++entry->header_fields;
            entry->header_stage = HTTP_HEADER_STAGE_START;
            continue;
        }

        if (entry->header_stage == HTTP_HEADER_STAGE_BLANK_CR) {
            if (value != '\n') {
                error = HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION;
                break;
            }
            entry->header_data[entry->header_stored++] = value;
            entry->header_complete = true;
            break;
        }

        error = HTTP_PARSER_BAD_STATE;
        break;
    }

    if (entry->header_stored != previous_stored) {
        http_header_publish_progress(
            entry,
            entry->header_complete
                ? HTTP_PARSER_STATE_COMPLETE
                : HTTP_PARSER_STATE_HEADERS);
    }
    if (error != HTTP_PARSER_OK) {
        return http_header_fail(entry, error);
    }
    *complete = entry->header_complete;
    return HTTP_PARSER_OK;
}

static enum http_parser_status http_header_field_at(
    const struct http_parser_entry *entry,
    uint32_t index,
    struct http_header_field *field)
{
    uint32_t offset = 0U;
    uint32_t current = 0U;

    while (offset < entry->header_stored) {
        uint32_t name_start = offset;
        uint32_t name_length = 0U;
        uint32_t value_start;
        uint32_t value_length = 0U;

        if (entry->header_data[offset] == '\r') {
            break;
        }
        while (offset < entry->header_stored &&
               entry->header_data[offset] != ':') {
            ++offset;
            ++name_length;
        }
        if (offset >= entry->header_stored ||
            name_length == 0U) {
            return HTTP_PARSER_BAD_STATE;
        }
        ++offset;
        value_start = offset;
        while (offset < entry->header_stored &&
               entry->header_data[offset] != '\r') {
            ++offset;
            ++value_length;
        }
        if (offset + 1U >= entry->header_stored ||
            entry->header_data[offset] != '\r' ||
            entry->header_data[offset + 1U] != '\n') {
            return HTTP_PARSER_BAD_STATE;
        }
        offset += 2U;
        if (current == index) {
            field->name = entry->header_data + name_start;
            field->name_length = name_length;
            field->value = entry->header_data + value_start;
            field->value_length = value_length;
            return HTTP_PARSER_OK;
        }
        ++current;
    }
    return HTTP_PARSER_HEADER_OUT_OF_RANGE;
}

enum http_parser_status http_header_count(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    uint32_t *count)
{
    const struct http_parser_entry *entry;
    enum http_parser_status token_status;

    if (parser == NULL || token == NULL || count == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_request_line_validate_const(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (entry->header_error != HTTP_PARSER_OK) {
        return entry->header_error;
    }
    if (!entry->header_complete) {
        return HTTP_PARSER_HEADER_INCOMPLETE;
    }
    *count = entry->header_fields;
    return HTTP_PARSER_OK;
}

enum http_parser_status http_header_field(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    uint32_t index,
    struct http_header_field *field)
{
    const struct http_parser_entry *entry;
    enum http_parser_status token_status;

    if (parser == NULL || token == NULL || field == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_request_line_validate_const(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (entry->header_error != HTTP_PARSER_OK) {
        return entry->header_error;
    }
    if (!entry->header_complete) {
        return HTTP_PARSER_HEADER_INCOMPLETE;
    }
    return http_header_field_at(entry, index, field);
}

enum http_parser_status http_header_error(
    const struct http_parser *parser,
    const struct http_parser_token *token)
{
    const struct http_parser_entry *entry;
    enum http_parser_status token_status;

    if (parser == NULL || token == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_request_line_validate_const(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    return entry->header_error;
}

#if defined(HTTP_POLICY_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static bool http_policy_name_equal(
    const uint8_t *name,
    uint32_t length,
    const char *expected)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        uint8_t left = name[index];

        if (expected[index] == '\0') {
            return false;
        }

        {
            uint8_t right = (uint8_t)expected[index];

            if (left >= 'A' && left <= 'Z') {
                left = (uint8_t)(left + ('a' - 'A'));
            }
            if (right >= 'A' && right <= 'Z') {
                right = (uint8_t)(right + ('a' - 'A'));
            }
            if (left != right) {
                return false;
            }
        }
    }
    return expected[length] == '\0';
}

static bool http_policy_exact_equal(
    const uint8_t *value,
    uint32_t length,
    const char *expected)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        if (expected[index] == '\0') {
            return false;
        }
        if (value[index] != (uint8_t)expected[index]) {
            return false;
        }
    }
    return expected[length] == '\0';
}

static void http_policy_trim_ows(
    const uint8_t *value,
    uint32_t value_length,
    const uint8_t **trimmed,
    uint32_t *trimmed_length)
{
    uint32_t start = 0U;
    uint32_t end = value_length;

    while (start < end &&
           (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    while (end > start &&
           (value[end - 1U] == ' ' ||
            value[end - 1U] == '\t')) {
        --end;
    }
    *trimmed = value + start;
    *trimmed_length = end - start;
}

static bool http_policy_host_byte(uint8_t value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') ||
           value == '-' ||
           value == '.' ||
           value == '_' ||
           value == '~';
}

static bool http_policy_host_valid(
    const uint8_t *value,
    uint32_t value_length)
{
    uint32_t index;
    uint32_t colon_count = 0U;
    uint32_t colon_index = 0U;

    if (value_length == 0U) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        uint8_t current = value[index];

        if (current == ':') {
            ++colon_count;
            colon_index = index;
            continue;
        }
        if (!http_policy_host_byte(current)) {
            return false;
        }
    }
    if (colon_count == 0U) {
        return true;
    }
    if (colon_count != 1U ||
        colon_index == 0U ||
        colon_index + 1U >= value_length) {
        return false;
    }
    for (index = colon_index + 1U;
         index < value_length;
         ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
    }
    return true;
}

const char *http_method_kind_string(
    enum http_method_kind kind)
{
    switch (kind) {
    case HTTP_METHOD_NONE:
        return "HTTP_METHOD_NONE";
    case HTTP_METHOD_GET:
        return "HTTP_METHOD_GET";
    case HTTP_METHOD_HEAD:
        return "HTTP_METHOD_HEAD";
    case HTTP_METHOD_UNSUPPORTED:
        return "HTTP_METHOD_UNSUPPORTED";
    case HTTP_METHOD_POST:
        return "HTTP_METHOD_POST";
    default:
        return NULL;
    }
}

const char *http_policy_reason_string(
    enum http_policy_reason reason)
{
    switch (reason) {
    case HTTP_POLICY_NONE:
        return "HTTP_POLICY_NONE";
    case HTTP_POLICY_METHOD:
        return "HTTP_POLICY_METHOD";
    case HTTP_POLICY_VERSION:
        return "HTTP_POLICY_VERSION";
    case HTTP_POLICY_MISSING_HOST:
        return "HTTP_POLICY_MISSING_HOST";
    case HTTP_POLICY_EMPTY_HOST:
        return "HTTP_POLICY_EMPTY_HOST";
    case HTTP_POLICY_DUPLICATE_HOST:
        return "HTTP_POLICY_DUPLICATE_HOST";
    case HTTP_POLICY_INVALID_HOST:
        return "HTTP_POLICY_INVALID_HOST";
    default:
        return NULL;
    }
}

/*
 * ITEM-445 step 2: the targets that accept a POST body, and nothing else.
 * `POST /` is the sign-in form, `POST /panel/logout` the sign-out form and
 * ITEM-503 adds the two certificate-import entries; ITEM-505 increment 3 adds
 * `/panel/system` and `/api/v1/system`, the two entries to the hostname
 * mutation; ITEM-506 increment 6 adds `/panel/account` and
 * `/api/v1/system/account`, the two entries to the credential change;
 * ITEM-513 adds `/panel/language`, the nav's one-click language switch, whose
 * body is one field and whose answer is one cookie.  Every
 * other target keeps answering 405 for POST exactly as it did
 * before, which keeps the frozen method matrix meaningful.
 *
 * The comparison is a table, not a chain of early returns: `/panel/routing`
 * and `/panel/network` have the same length, and a length-first chain that
 * returns on the first match would answer for the wrong target.  ITEM-505
 * found exactly that when the network targets were appended after the routing
 * check.
 */
static bool http_policy_form_target(
    const uint8_t *target,
    uint32_t target_length)
{
    static const struct {
        const char *value;
        uint32_t length;
    } targets[] = {
        {HTTP_POLICY_SIGN_OUT_TARGET, HTTP_POLICY_SIGN_OUT_TARGET_LENGTH},
        {HTTP_POLICY_SECURITY_TARGET, HTTP_POLICY_SECURITY_TARGET_LENGTH},
        {HTTP_POLICY_CERTIFICATE_TARGET,
         HTTP_POLICY_CERTIFICATE_TARGET_LENGTH},
        {HTTP_POLICY_ROUTING_TARGET, HTTP_POLICY_ROUTING_TARGET_LENGTH},
        {HTTP_POLICY_ROUTES_TARGET, HTTP_POLICY_ROUTES_TARGET_LENGTH},
        {HTTP_POLICY_NETWORK_TARGET, HTTP_POLICY_NETWORK_TARGET_LENGTH},
        {HTTP_POLICY_PANEL_NETWORK_TARGET,
         HTTP_POLICY_PANEL_NETWORK_TARGET_LENGTH},
        {HTTP_POLICY_SYSTEM_TARGET, HTTP_POLICY_SYSTEM_TARGET_LENGTH},
        {HTTP_POLICY_PANEL_SYSTEM_TARGET,
         HTTP_POLICY_PANEL_SYSTEM_TARGET_LENGTH},
        {HTTP_POLICY_ACCOUNT_TARGET, HTTP_POLICY_ACCOUNT_TARGET_LENGTH},
        {HTTP_POLICY_PANEL_ACCOUNT_TARGET,
         HTTP_POLICY_PANEL_ACCOUNT_TARGET_LENGTH},
        {HTTP_POLICY_CERTIFICATE_NAME_TARGET,
         HTTP_POLICY_CERTIFICATE_NAME_TARGET_LENGTH},
        {HTTP_POLICY_PANEL_CERTIFICATES_TARGET,
         HTTP_POLICY_PANEL_CERTIFICATES_TARGET_LENGTH},
        /* ITEM-508 increment 34: the ACME control page and its API target. */
        {HTTP_POLICY_ACME_TARGET, HTTP_POLICY_ACME_TARGET_LENGTH},
        {HTTP_POLICY_PANEL_ACME_TARGET,
         HTTP_POLICY_PANEL_ACME_TARGET_LENGTH},
        /* ITEM-513: the nav's one-click language switch. */
        {HTTP_POLICY_PANEL_LANGUAGE_TARGET,
         HTTP_POLICY_PANEL_LANGUAGE_TARGET_LENGTH},
        /* ITEM-516: the management gate's page and its API target. */
        {HTTP_POLICY_GATE_TARGET, HTTP_POLICY_GATE_TARGET_LENGTH},
        {HTTP_POLICY_PANEL_GATE_TARGET,
         HTTP_POLICY_PANEL_GATE_TARGET_LENGTH},
    };
    size_t index;

    if (target == NULL) {
        return false;
    }
    if (target_length == HTTP_POLICY_FORM_TARGET_LENGTH) {
        return target[0] == (uint8_t)'/';
    }
    for (index = 0U; index < sizeof(targets) / sizeof(targets[0]); ++index) {
        if (target_length == targets[index].length &&
            memcmp(target, targets[index].value, target_length) == 0) {
            return true;
        }
    }
    return false;
}

enum http_parser_status http_policy_check(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_policy_result *result)
{
    const struct http_parser_entry *entry;
    enum http_parser_status token_status;
    struct http_request_line line;
    uint32_t index;
    uint32_t host_count = 0U;
    uint32_t host_field = 0U;
    uint32_t duplicate_field = 0U;
    enum http_policy_reason host_reason = HTTP_POLICY_NONE;
    uint32_t host_reason_field = 0U;

    if (parser == NULL || token == NULL || result == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_request_line_validate_const(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (entry->request_line_error != HTTP_PARSER_OK) {
        return entry->request_line_error;
    }
    if (entry->header_error != HTTP_PARSER_OK) {
        return entry->header_error;
    }
    if (entry->state == HTTP_PARSER_STATE_REJECTED) {
        return HTTP_PARSER_TERMINAL;
    }
    if (entry->state != HTTP_PARSER_STATE_COMPLETE ||
        !entry->header_complete) {
        return HTTP_PARSER_HEADER_INCOMPLETE;
    }
    token_status = http_request_line_get(
        parser, token, &line);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }

    memset(result, 0, sizeof(*result));
    result->method = HTTP_METHOD_UNSUPPORTED;
    if (http_policy_exact_equal(
            line.method,
            line.method_length,
            "GET")) {
        result->method = HTTP_METHOD_GET;
    } else if (http_policy_exact_equal(
                   line.method,
                   line.method_length,
                   "HEAD")) {
        result->method = HTTP_METHOD_HEAD;
    } else if (http_policy_exact_equal(
                   line.method,
                   line.method_length,
                   "POST")) {
        result->method = HTTP_METHOD_POST;
    }
    result->method_allowed =
        result->method == HTTP_METHOD_GET ||
        result->method == HTTP_METHOD_HEAD ||
        (result->method == HTTP_METHOD_POST &&
         http_policy_form_target(line.target, line.target_length));
    result->version_allowed =
        http_policy_exact_equal(
            line.version,
            line.version_length,
            "HTTP/1.1");

    for (index = 0U; index < entry->header_fields; ++index) {
        struct http_header_field field;
        enum http_parser_status field_status;

        field_status = http_header_field_at(entry, index, &field);
        if (field_status != HTTP_PARSER_OK) {
            return field_status;
        }
        if (http_policy_name_equal(
                field.name,
                field.name_length,
                "Host")) {
            if (host_count == 0U) {
                host_field = index;
            } else if (host_count == 1U) {
                duplicate_field = index;
            }
            ++host_count;
        }
    }

    if (host_count == 0U) {
        host_reason = HTTP_POLICY_MISSING_HOST;
        host_reason_field = 0U;
    } else if (host_count > 1U) {
        host_reason = HTTP_POLICY_DUPLICATE_HOST;
        host_reason_field = duplicate_field;
    } else {
        struct http_header_field field;
        const uint8_t *trimmed;
        uint32_t trimmed_length;
        enum http_parser_status field_status;

        field_status = http_header_field_at(
            entry, host_field, &field);
        if (field_status != HTTP_PARSER_OK) {
            return field_status;
        }
        http_policy_trim_ows(
            field.value,
            field.value_length,
            &trimmed,
            &trimmed_length);
        host_reason_field = host_field;
        if (trimmed_length == 0U) {
            host_reason = HTTP_POLICY_EMPTY_HOST;
        } else if (!http_policy_host_valid(
                       trimmed,
                       trimmed_length)) {
            host_reason = HTTP_POLICY_INVALID_HOST;
        } else {
            result->host_valid = true;
            host_reason = HTTP_POLICY_NONE;
        }
    }

    if (!result->method_allowed) {
        result->allowed = false;
        result->reason = HTTP_POLICY_METHOD;
        result->field_index = 0U;
        return HTTP_PARSER_OK;
    }
    if (!result->version_allowed) {
        result->allowed = false;
        result->reason = HTTP_POLICY_VERSION;
        result->field_index = 0U;
        return HTTP_PARSER_OK;
    }
    if (!result->host_valid) {
        result->allowed = false;
        result->reason = host_reason;
        result->field_index = host_reason_field;
        return HTTP_PARSER_OK;
    }
    result->allowed = true;
    result->reason = HTTP_POLICY_NONE;
    result->field_index = 0U;
    return HTTP_PARSER_OK;
}

#endif

#if defined(HTTP_FRAMING_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static bool http_framing_name_equal(
    const uint8_t *name,
    uint32_t length,
    const char *expected)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        uint8_t left = name[index];

        if (expected[index] == '\0') {
            return false;
        }

        {
            uint8_t right = (uint8_t)expected[index];

            if (left >= 'A' && left <= 'Z') {
                left = (uint8_t)(left + ('a' - 'A'));
            }
            if (right >= 'A' && right <= 'Z') {
                right = (uint8_t)(right + ('a' - 'A'));
            }
            if (left != right) {
                return false;
            }
        }
    }
    return expected[length] == '\0';
}

static void http_framing_trim_ows(
    const uint8_t *value,
    uint32_t value_length,
    const uint8_t **trimmed,
    uint32_t *trimmed_length)
{
    uint32_t start = 0U;
    uint32_t end = value_length;

    while (start < end &&
           (value[start] == ' ' || value[start] == '\t')) {
        ++start;
    }
    while (end > start &&
           (value[end - 1U] == ' ' ||
            value[end - 1U] == '\t')) {
        --end;
    }
    *trimmed = value + start;
    *trimmed_length = end - start;
}

static bool http_framing_content_length_valid(
    const uint8_t *value,
    uint32_t value_length)
{
    const uint8_t *trimmed;
    uint32_t length;
    uint32_t index;

    http_framing_trim_ows(
        value, value_length, &trimmed, &length);
    if (length == 0U) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (trimmed[index] < '0' ||
            trimmed[index] > '9') {
            return false;
        }
    }
    return true;
}

/*
 * ITEM-445 step 2 prototype: turn a validated Content-Length into a number. The
 * digits-only check above is not enough: a naive accumulate wraps, and
 * `Content-Length: 4294967808` (2^32 + 512) would then be read as the 512 the
 * policy allows. Overflow is refused before the multiply, so an oversized value
 * stays denied instead of aliasing a permitted body.
 */
static bool http_framing_content_length_parse(
    const uint8_t *value,
    uint32_t value_length,
    uint32_t *parsed_out)
{
    const uint8_t *trimmed;
    uint32_t length;
    uint32_t index;
    uint32_t parsed = 0U;

    if (parsed_out == NULL) {
        return false;
    }
    http_framing_trim_ows(
        value, value_length, &trimmed, &length);
    if (length == 0U) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        uint32_t digit;

        if (trimmed[index] < '0' ||
            trimmed[index] > '9') {
            return false;
        }
        digit = (uint32_t)(trimmed[index] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *parsed_out = parsed;
    return true;
}

static bool http_framing_transfer_encoding_valid(
    const uint8_t *value,
    uint32_t value_length)
{
    const uint8_t *trimmed;
    uint32_t length;
    uint32_t offset = 0U;
    bool saw_coding = false;

    http_framing_trim_ows(
        value, value_length, &trimmed, &length);

    while (offset < length) {
        uint32_t start;

        while (offset < length &&
               (trimmed[offset] == ' ' ||
                trimmed[offset] == '\t')) {
            ++offset;
        }
        if (offset >= length) {
            return false;
        }
        if (trimmed[offset] == ',') {
            return false;
        }
        start = offset;
        while (offset < length &&
               trimmed[offset] != ',' &&
               trimmed[offset] != ' ' &&
               trimmed[offset] != '\t') {
            if (!http_request_line_tchar(trimmed[offset])) {
                return false;
            }
            ++offset;
        }
        if (offset == start) {
            return false;
        }
        saw_coding = true;
        while (offset < length &&
               (trimmed[offset] == ' ' ||
                trimmed[offset] == '\t')) {
            ++offset;
        }
        if (offset >= length) {
            return true;
        }
        if (trimmed[offset] != ',') {
            return false;
        }
        ++offset;
        if (offset >= length) {
            return false;
        }
    }
    return saw_coding;
}

const char *http_framing_kind_string(
    enum http_framing_kind kind)
{
    switch (kind) {
    case HTTP_FRAMING_NONE:
        return "HTTP_FRAMING_NONE";
    case HTTP_FRAMING_CONTENT_LENGTH:
        return "HTTP_FRAMING_CONTENT_LENGTH";
    case HTTP_FRAMING_TRANSFER_ENCODING:
        return "HTTP_FRAMING_TRANSFER_ENCODING";
    case HTTP_FRAMING_EXPECT:
        return "HTTP_FRAMING_EXPECT";
    case HTTP_FRAMING_UPGRADE:
        return "HTTP_FRAMING_UPGRADE";
    case HTTP_FRAMING_DUPLICATE:
        return "HTTP_FRAMING_DUPLICATE";
    case HTTP_FRAMING_CONFLICT:
        return "HTTP_FRAMING_CONFLICT";
    case HTTP_FRAMING_INVALID:
        return "HTTP_FRAMING_INVALID";
    default:
        return NULL;
    }
}

enum http_parser_status http_framing_check(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct http_framing_result *result)
{
    const struct http_parser_entry *entry;
    enum http_parser_status token_status;
    uint32_t index;
    uint32_t content_length_count = 0U;
    uint32_t transfer_encoding_count = 0U;
    uint32_t content_length_first = 0U;
    uint32_t content_length_second = 0U;
    uint32_t transfer_encoding_first = 0U;
    uint32_t transfer_encoding_second = 0U;
    uint32_t expect_field = 0U;
    uint32_t upgrade_field = 0U;
    uint32_t invalid_field = 0U;
    bool body_seen = false;
    bool upgrade_seen = false;
    bool invalid = false;
    enum http_framing_kind body_kind = HTTP_FRAMING_NONE;

    if (parser == NULL || token == NULL || result == NULL) {
        return HTTP_PARSER_NULL;
    }
    token_status = http_request_line_validate_const(
        parser, token, &entry);
    if (token_status != HTTP_PARSER_OK) {
        return token_status;
    }
    if (entry->header_error != HTTP_PARSER_OK) {
        return entry->header_error;
    }
    if (entry->state == HTTP_PARSER_STATE_REJECTED) {
        return HTTP_PARSER_TERMINAL;
    }
    if (entry->state != HTTP_PARSER_STATE_COMPLETE ||
        !entry->header_complete) {
        return HTTP_PARSER_HEADER_INCOMPLETE;
    }

    memset(result, 0, sizeof(*result));
    for (index = 0U; index < entry->header_fields; ++index) {
        struct http_header_field field;
        enum http_parser_status field_status;

        field_status = http_header_field_at(entry, index, &field);
        if (field_status != HTTP_PARSER_OK) {
            return field_status;
        }
        if (http_framing_name_equal(
                field.name,
                field.name_length,
                "Content-Length")) {
            if (content_length_count == 0U) {
                content_length_first = index;
            } else if (content_length_count == 1U) {
                content_length_second = index;
            }
            ++content_length_count;
            if (!http_framing_content_length_valid(
                    field.value,
                    field.value_length)) {
                if (!invalid) {
                    invalid_field = index;
                }
                invalid = true;
            }
            continue;
        }
        if (http_framing_name_equal(
                field.name,
                field.name_length,
                "Transfer-Encoding")) {
            if (transfer_encoding_count == 0U) {
                transfer_encoding_first = index;
            } else if (transfer_encoding_count == 1U) {
                transfer_encoding_second = index;
            }
            ++transfer_encoding_count;
            if (!http_framing_transfer_encoding_valid(
                    field.value,
                    field.value_length)) {
                if (!invalid) {
                    invalid_field = index;
                }
                invalid = true;
            }
            continue;
        }
        if (http_framing_name_equal(
                field.name,
                field.name_length,
                "Expect") &&
            !body_seen) {
            body_seen = true;
            body_kind = HTTP_FRAMING_EXPECT;
            expect_field = index;
            continue;
        }
        if (http_framing_name_equal(
                field.name,
                field.name_length,
                "Upgrade") &&
            !upgrade_seen) {
            upgrade_seen = true;
            upgrade_field = index;
        }
    }

    if (invalid) {
        result->body_denied = true;
        result->reason = HTTP_FRAMING_INVALID;
        result->field_index = invalid_field;
        return HTTP_PARSER_OK;
    }
    if (content_length_count > 1U ||
        transfer_encoding_count > 1U) {
        result->body_denied = true;
        result->reason = HTTP_FRAMING_DUPLICATE;
        result->field_index =
            content_length_count > 1U
                ? content_length_second
                : transfer_encoding_second;
        return HTTP_PARSER_OK;
    }
    if (content_length_count > 0U &&
        transfer_encoding_count > 0U) {
        result->body_denied = true;
        result->reason = HTTP_FRAMING_CONFLICT;
        result->field_index = transfer_encoding_first;
        return HTTP_PARSER_OK;
    }
    if (content_length_count > 0U) {
        const uint8_t *method =
            entry->request_line_data +
            entry->request_line_fields.method_offset;
        struct http_header_field length_field;
        uint32_t length_value = 0U;
        bool method_post =
            entry->request_line_fields.method_length == 4U &&
            memcmp(method, "POST", 4U) == 0;
        bool accepted =
            method_post &&
            http_header_field_at(
                entry,
                content_length_first,
                &length_field) == HTTP_PARSER_OK &&
            http_framing_content_length_parse(
                length_field.value,
                length_field.value_length,
                &length_value) &&
            length_value <= HTTP_FRAMING_BODY_MAX;

        if (!accepted) {
            result->body_denied = true;
            result->reason = HTTP_FRAMING_CONTENT_LENGTH;
            result->field_index = content_length_first;
            return HTTP_PARSER_OK;
        }
        /*
         * A bounded POST body is framed, not denied: the caller now owns
         * exactly `content_length` bytes and must not complete the request
         * before it has consumed them. The Expect and Upgrade checks below
         * still apply.
         */
        result->body_present = true;
        result->content_length = length_value;
    }
    if (transfer_encoding_count > 0U) {
        result->body_denied = true;
        result->reason = HTTP_FRAMING_TRANSFER_ENCODING;
        result->field_index = transfer_encoding_first;
        return HTTP_PARSER_OK;
    }
    if (body_seen) {
        result->body_denied = true;
        result->reason = body_kind;
        result->field_index = expect_field;
        return HTTP_PARSER_OK;
    }
    if (upgrade_seen) {
        result->upgrade_denied = true;
        result->reason = HTTP_FRAMING_UPGRADE;
        result->field_index = upgrade_field;
        return HTTP_PARSER_OK;
    }
    result->reason = HTTP_FRAMING_NONE;
    return HTTP_PARSER_OK;
}

#endif

#endif

#if defined(HTTP_ROUTES_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static const struct http_route_descriptor http_route_table_data[] = {
    {
        HTTP_ROUTE_ROOT,
        (const uint8_t *)"/",
        HTTP_ROUTE_ROOT_LENGTH,
    },
    {
        HTTP_ROUTE_HEALTH,
        (const uint8_t *)"/health",
        HTTP_ROUTE_HEALTH_LENGTH,
    },
    /* ITEM-503: the active certificate as PEM, public like /health. */
    {
        HTTP_ROUTE_CERTIFICATE,
        (const uint8_t *)"/hfoc-ca.crt",
        HTTP_ROUTE_CERTIFICATE_LENGTH,
    },
};

static bool http_route_exact_equal(
    const uint8_t *target,
    uint32_t target_length,
    const char *expected)
{
    uint32_t index;

    for (index = 0U; index < target_length; ++index) {
        if (expected[index] == '\0') {
            return false;
        }
        if (target[index] != (uint8_t)expected[index]) {
            return false;
        }
    }
    return expected[target_length] == '\0';
}

enum http_route_status http_route_table(
    const struct http_route_descriptor **routes,
    uint32_t *count)
{
    if (routes == NULL || count == NULL) {
        return HTTP_ROUTE_NULL;
    }
    *routes = http_route_table_data;
    *count = HTTP_ROUTE_COUNT;
    return HTTP_ROUTE_OK;
}

enum http_route_status http_route_match(
    const uint8_t *target,
    uint32_t target_length,
    enum http_route_id *route)
{
    if (target == NULL || route == NULL) {
        return HTTP_ROUTE_NULL;
    }
    *route = HTTP_ROUTE_NONE;
    if (http_route_exact_equal(
            target,
            target_length,
            "/")) {
        *route = HTTP_ROUTE_ROOT;
    } else if (http_route_exact_equal(
                   target,
                   target_length,
                   "/health")) {
        *route = HTTP_ROUTE_HEALTH;
    } else if (http_route_exact_equal(
                   target,
                   target_length,
                   "/hfoc-ca.crt")) {
        /* ITEM-503: the public certificate document. */
        *route = HTTP_ROUTE_CERTIFICATE;
    }
    return HTTP_ROUTE_OK;
}

enum http_route_status http_route_match_line(
    const struct http_request_line *line,
    enum http_route_id *route)
{
    if (line == NULL || route == NULL) {
        return HTTP_ROUTE_NULL;
    }
    return http_route_match(
        line->target,
        line->target_length,
        route);
}

const char *http_route_id_string(
    enum http_route_id route)
{
    switch (route) {
    case HTTP_ROUTE_NONE:
        return "HTTP_ROUTE_NONE";
    case HTTP_ROUTE_ROOT:
        return "HTTP_ROUTE_ROOT";
    case HTTP_ROUTE_HEALTH:
        return "HTTP_ROUTE_HEALTH";
    case HTTP_ROUTE_CERTIFICATE:
        return "HTTP_ROUTE_CERTIFICATE";
    default:
        return NULL;
    }
}

#endif

#endif

#else

typedef int http_parser_unused;

#endif
