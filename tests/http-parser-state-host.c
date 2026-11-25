#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HTTP_PARSER_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct http_parser_token make_token(
    uint32_t index,
    uint32_t generation)
{
    struct http_parser_token token;

    memset(&token, 0, sizeof(token));
    token.index = index;
    token.generation = generation;
    return token;
}

static struct http_parser_progress make_progress(
    uint32_t request_line_offset,
    uint32_t request_line_count,
    uint32_t header_offset,
    uint32_t header_count,
    uint32_t total_count)
{
    struct http_parser_progress progress;

    memset(&progress, 0, sizeof(progress));
    progress.request_line_offset = request_line_offset;
    progress.request_line_count = request_line_count;
    progress.header_offset = header_offset;
    progress.header_count = header_count;
    progress.total_count = total_count;
    return progress;
}

static struct http_parser_progress empty_progress(void)
{
    return make_progress(0U, 0U, 0U, 0U, 0U);
}

static struct http_parser_progress request_line_progress(
    uint32_t count)
{
    return make_progress(0U, count, 0U, 0U, count);
}

static struct http_parser_progress header_start_progress(
    uint32_t request_line_count)
{
    return make_progress(
        0U,
        request_line_count,
        request_line_count,
        0U,
        request_line_count);
}

static struct http_parser_progress header_progress(
    uint32_t request_line_count,
    uint32_t header_count)
{
    return make_progress(
        0U,
        request_line_count,
        request_line_count,
        header_count,
        request_line_count + header_count);
}

static bool init_parser(struct http_parser *parser)
{
    memset(parser, 0, sizeof(*parser));
    return http_parser_init(parser) == HTTP_PARSER_OK;
}

static bool progress_equal(
    const struct http_parser_progress *left,
    const struct http_parser_progress *right)
{
    return left != NULL &&
           right != NULL &&
           left->request_line_offset == right->request_line_offset &&
           left->request_line_count == right->request_line_count &&
           left->header_offset == right->header_offset &&
           left->header_count == right->header_count &&
           left->total_count == right->total_count;
}

static bool active_empty(
    const struct http_parser *parser,
    uint32_t index,
    uint32_t generation)
{
    const struct http_parser_entry *entry;
    struct http_parser_progress progress;

    if (parser == NULL || index >= HTTP_PARSER_SLOT_COUNT) {
        return false;
    }
    entry = &parser->entries[index];
    return entry->active &&
           entry->generation == generation &&
           entry->state == HTTP_PARSER_STATE_EMPTY &&
           http_parser_progress_at(
               parser, index, &progress) == HTTP_PARSER_OK &&
           progress_equal(
               &progress,
               &(struct http_parser_progress){
                   0U, 0U, 0U, 0U, 0U
               });
}

static void test_strings_and_unpublished(void)
{
    struct http_parser parser;
    struct http_parser_progress progress;
    enum http_parser_state state;
    enum http_parser_status status;

    memset(&parser, 0, sizeof(parser));
    CHECK(!http_parser_published(&parser) &&
              http_parser_active_count(&parser) == 0U &&
              http_parser_request_line_max(&parser) == 0U &&
              http_parser_headers_max(&parser) == 0U,
          "unpublished parser accessors fail closed");
    CHECK(http_parser_state_at(&parser, 0U) ==
              HTTP_PARSER_STATE_EMPTY &&
              http_parser_generation_at(&parser, 0U) == 0U &&
              http_parser_progress_at(
                  &parser, 0U, &progress) ==
                  HTTP_PARSER_BAD_STATE,
          "unpublished parser slot accessors fail closed");

    for (state = HTTP_PARSER_STATE_EMPTY;
         state <= HTTP_PARSER_STATE_REJECTED;
         ++state) {
        CHECK(http_parser_state_string(state) != NULL,
              "parser state string is defined for every state");
    }
    CHECK(http_parser_state_string(
              (enum http_parser_state)99U) == NULL,
          "parser undefined state string is rejected");
    for (status = HTTP_PARSER_OK;
         status <= HTTP_PARSER_TERMINAL;
         ++status) {
        CHECK(http_parser_status_string(status) != NULL,
              "parser status string is defined for every status");
    }
    CHECK(http_parser_status_string(
              (enum http_parser_status)99U) == NULL,
          "parser undefined status string is rejected");
}

static void test_init_start_release(void)
{
    struct http_parser parser;
    struct http_parser_progress progress;
    struct http_parser_token token;
    struct http_parser_token stale_token;
    uint32_t index;

    CHECK(http_parser_init(NULL) == HTTP_PARSER_NULL,
          "parser init rejects null parser");
    CHECK(init_parser(&parser),
          "valid parser init publishes fixed descriptor");
    CHECK(http_parser_init(&parser) == HTTP_PARSER_BAD_STATE,
          "published parser rejects double init");
    CHECK(http_parser_published(&parser) &&
              parser.version == HTTP_PARSER_VERSION &&
              parser.active_count == 0U &&
              http_parser_active_count(&parser) == 0U &&
              http_parser_request_line_max(&parser) ==
                  HTTP_PARSER_REQUEST_LINE_MAX &&
              http_parser_headers_max(&parser) ==
                  HTTP_PARSER_HEADERS_MAX,
          "published parser exposes fixed limits and empty slots");
    for (index = 0U; index < HTTP_PARSER_SLOT_COUNT; ++index) {
        CHECK(http_parser_state_at(&parser, index) ==
                  HTTP_PARSER_STATE_EMPTY &&
                  http_parser_generation_at(&parser, index) == 0U &&
                  http_parser_progress_at(
                      &parser, index, &progress) ==
                      HTTP_PARSER_OK &&
                  progress_equal(
                      &progress,
                      &(struct http_parser_progress){
                          0U, 0U, 0U, 0U, 0U
                      }),
              "published parser seeds four free empty slots");
    }

    token = make_token(0U, HTTP_PARSER_GENERATION_MIN);
    CHECK(http_parser_start(&parser, NULL) == HTTP_PARSER_NULL,
          "parser start rejects null token");
    token.generation = 0U;
    CHECK(http_parser_start(&parser, &token) ==
              HTTP_PARSER_BAD_TOKEN,
          "parser start rejects zero generation");
    token.generation = HTTP_PARSER_GENERATION_MAX + 1U;
    CHECK(http_parser_start(&parser, &token) ==
              HTTP_PARSER_BAD_TOKEN,
          "parser start rejects invalid generation");
    token.generation = HTTP_PARSER_GENERATION_MIN;
    token.index = HTTP_PARSER_SLOT_COUNT;
    CHECK(http_parser_start(&parser, &token) ==
              HTTP_PARSER_BAD_TOKEN,
          "parser start rejects out-of-range slot");

    token.index = 0U;
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              active_empty(&parser, 0U, HTTP_PARSER_GENERATION_MIN) &&
              parser.active_count == 1U,
          "parser start publishes empty generation-tagged slot");
    stale_token = make_token(0U, HTTP_PARSER_GENERATION_MIN + 1U);
    CHECK(http_parser_start(&parser, &stale_token) ==
              HTTP_PARSER_STALE_TOKEN,
          "parser start rejects stale generation");
    CHECK(http_parser_start(&parser, &token) ==
              HTTP_PARSER_ALREADY_ATTACHED,
          "parser start rejects duplicate attachment");

    CHECK(http_parser_release(&parser, &stale_token) ==
              HTTP_PARSER_STALE_TOKEN,
          "parser release rejects stale generation");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              !parser.entries[0].active &&
              parser.active_count == 0U,
          "parser release restores slot to free and updates count");
    CHECK(http_parser_release(&parser, &token) ==
              HTTP_PARSER_NOT_ATTACHED,
          "parser release rejects released slot");
}

static void test_exact_one_way_publication(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_progress progress;
    struct http_parser_progress before;
    struct http_parser_progress after;
    struct http_parser_progress expected;
    struct http_parser_progress invalid;

    CHECK(init_parser(&parser),
          "one-way fixture initializes parser");
    token = make_token(0U, 7U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              active_empty(&parser, 0U, 7U),
          "one-way fixture starts slot seven");

    expected = request_line_progress(8U);
    CHECK(http_parser_progress_at(
              &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(
                  &progress,
                  &(struct http_parser_progress){
                      0U, 0U, 0U, 0U, 0U
                  }) &&
              http_parser_advance(
                  &parser,
                  &token,
                  HTTP_PARSER_STATE_REQUEST_LINE,
                  &expected) == HTTP_PARSER_OK &&
              parser.entries[0].state ==
                  HTTP_PARSER_STATE_REQUEST_LINE &&
              http_parser_state(&parser, &token) ==
                  HTTP_PARSER_STATE_REQUEST_LINE &&
              http_parser_generation(&parser, &token) == 7U &&
              http_parser_progress_at(
                  &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &expected),
          "EMPTY advances one way to REQUEST_LINE with exact counts");

    before = expected;
    expected = header_start_progress(8U);
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_HEADERS,
              &expected) == HTTP_PARSER_OK &&
              parser.entries[0].state ==
                  HTTP_PARSER_STATE_HEADERS &&
              http_parser_progress_at(
                  &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &expected),
          "REQUEST_LINE advances one way to HEADERS with exact offset");

    invalid = request_line_progress(8U);
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &invalid) == HTTP_PARSER_INVALID_PREDECESSOR &&
              parser.entries[0].state ==
                  HTTP_PARSER_STATE_HEADERS &&
              http_parser_progress_at(
                  &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &before) == false &&
              progress_equal(&progress, &expected),
          "HEADERS rejects backward REQUEST_LINE predecessor");

    before = expected;
    expected = header_progress(8U, 12U);
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_HEADERS,
              &expected) == HTTP_PARSER_OK &&
              parser.entries[0].state ==
                  HTTP_PARSER_STATE_HEADERS &&
              http_parser_progress_at(
                  &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &expected),
          "HEADERS accepts same-state monotonic header progress");

    before = expected;
    invalid = header_progress(8U, 12U);
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_HEADERS,
              &invalid) == HTTP_PARSER_INVALID_PROGRESS &&
              parser.entries[0].state ==
                  HTTP_PARSER_STATE_HEADERS &&
              http_parser_progress_at(
                  &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &before),
          "same-state advance requires a strict count increase");

    expected = header_progress(8U, 12U);
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_COMPLETE,
              &expected) == HTTP_PARSER_OK &&
              parser.entries[0].state ==
                  HTTP_PARSER_STATE_COMPLETE &&
              http_parser_state(&parser, &token) ==
                  HTTP_PARSER_STATE_COMPLETE &&
              http_parser_progress_at(
                  &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &expected),
          "HEADERS advances one way to COMPLETE with exact total");

    invalid = header_progress(8U, 13U);
    memcpy(&after, &expected, sizeof(after));
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_COMPLETE,
              &invalid) == HTTP_PARSER_TERMINAL &&
              http_parser_progress_at(
                  &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &after),
          "terminal COMPLETE rejects further publication");
}

static void test_incremental_and_invalid_progress(void)
{
    struct http_parser parser;
    struct http_parser_token token_struct;
    struct http_parser_token *token = &token_struct;
    struct http_parser_progress before;
    struct http_parser_progress progress;
    struct http_parser_progress expected;
    struct http_parser_progress invalid;

    CHECK(init_parser(&parser),
          "incremental fixture initializes parser");
    token_struct = make_token(1U, 9U);
    CHECK(http_parser_start(&parser, token) == HTTP_PARSER_OK,
          "incremental fixture starts slot one");

    expected = request_line_progress(3U);
    CHECK(http_parser_advance(
              &parser,
              token,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &expected) == HTTP_PARSER_OK,
          "request line accepts first bounded slice");
    before = expected;
    expected = request_line_progress(8U);
    CHECK(http_parser_advance(
              &parser,
              token,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &expected) == HTTP_PARSER_OK &&
              http_parser_progress_at(
                  &parser, 1U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &expected),
          "request line accepts monotonic next slice");

    invalid = request_line_progress(2U);
    CHECK(http_parser_advance(
              &parser,
              token,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &invalid) == HTTP_PARSER_INVALID_PROGRESS &&
              http_parser_progress_at(
                  &parser, 1U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &before) == false &&
              progress_equal(&progress, &expected),
          "request line rejects backward progress without mutation");

    invalid = make_progress(
        0U, 8U, 8U, 0U, 9U);
    CHECK(http_parser_advance(
              &parser,
              token,
              HTTP_PARSER_STATE_HEADERS,
              &invalid) == HTTP_PARSER_INVALID_PROGRESS &&
              parser.entries[1].state ==
                  HTTP_PARSER_STATE_REQUEST_LINE &&
              http_parser_progress_at(
                  &parser, 1U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &expected),
          "inconsistent total rejects header transition");

    invalid = make_progress(
        1U, 8U, 0U, 0U, 8U);
    CHECK(http_parser_advance(
              &parser,
              token,
              HTTP_PARSER_STATE_HEADERS,
              &invalid) == HTTP_PARSER_INVALID_PROGRESS &&
              parser.entries[1].state ==
                  HTTP_PARSER_STATE_REQUEST_LINE,
          "nonzero request-line offset rejects transition");

    expected = header_start_progress(8U);
    CHECK(http_parser_advance(
              &parser,
              token,
              HTTP_PARSER_STATE_HEADERS,
              &expected) == HTTP_PARSER_OK,
          "request line finalizes to HEADERS");
}

static void test_stale_generation_and_limits(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_token wrong;
    struct http_parser_progress before;
    struct http_parser_progress progress;
    struct http_parser_progress expected;
    struct http_parser_progress invalid;

    CHECK(init_parser(&parser),
          "limit fixture initializes parser");
    token = make_token(2U, 5U);
    wrong = make_token(2U, 4U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "limit fixture starts slot two");
    CHECK(http_parser_state(
              &parser, &wrong) == HTTP_PARSER_STATE_EMPTY &&
              http_parser_generation(&parser, &wrong) == 0U,
          "stale token accessor fails closed");
    CHECK(http_parser_progress(
              &parser, &wrong, &progress) ==
              HTTP_PARSER_STALE_TOKEN,
          "stale token progress accessor rejects");

    expected = request_line_progress(8U);
    CHECK(http_parser_advance(
              &parser,
              &wrong,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &expected) == HTTP_PARSER_STALE_TOKEN &&
              parser.entries[2].state ==
                  HTTP_PARSER_STATE_EMPTY,
          "stale token cannot advance parser");
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &expected) == HTTP_PARSER_OK,
          "valid token advances parser after stale denial");
    before = expected;

    invalid = request_line_progress(
        HTTP_PARSER_REQUEST_LINE_MAX + 1U);
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &invalid) == HTTP_PARSER_REQUEST_LINE_LIMIT &&
              http_parser_progress_at(
                  &parser, 2U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &before),
          "request line overflow rejects without mutation");

    expected = header_start_progress(8U);
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_HEADERS,
              &expected) == HTTP_PARSER_OK,
          "limit fixture enters HEADERS");
    before = expected;
    invalid = header_progress(
        8U, HTTP_PARSER_HEADERS_MAX + 1U);
    CHECK(http_parser_advance(
              &parser,
              &token,
              HTTP_PARSER_STATE_HEADERS,
              &invalid) == HTTP_PARSER_HEADERS_LIMIT &&
              http_parser_progress_at(
                  &parser, 2U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &before),
          "header overflow rejects without mutation");

    CHECK(http_parser_reject(
              &parser,
              &token,
              &before) == HTTP_PARSER_OK &&
              parser.entries[2].state ==
                  HTTP_PARSER_STATE_REJECTED,
          "bounded header progress can reject fail-closed");
}

static void test_rejection_and_slot_isolation(void)
{
    struct http_parser parser;
    struct http_parser_progress empty;
    struct http_parser_progress request_line;
    struct http_parser_progress headers;
    struct http_parser_token first;
    struct http_parser_token second;
    struct http_parser_token third;
    struct http_parser_token fourth;
    struct http_parser_progress progress;
    uint32_t index;

    CHECK(init_parser(&parser),
          "isolation fixture initializes parser");
    first = make_token(0U, 1U);
    second = make_token(1U, 2U);
    third = make_token(2U, 3U);
    fourth = make_token(3U, 4U);
    CHECK(http_parser_start(&parser, &first) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &second) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &third) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &fourth) == HTTP_PARSER_OK &&
              parser.active_count == HTTP_PARSER_SLOT_COUNT,
          "four parser slots attach independently");

    empty = empty_progress();
    request_line = request_line_progress(4U);
    CHECK(http_parser_reject(
              &parser, &first, &empty) == HTTP_PARSER_OK &&
              parser.entries[0].state ==
                  HTTP_PARSER_STATE_REJECTED,
          "EMPTY rejects to REJECTED");
    CHECK(http_parser_advance(
              &parser,
              &second,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &request_line) == HTTP_PARSER_OK &&
              http_parser_reject(
                  &parser,
                  &second,
                  &request_line) == HTTP_PARSER_OK &&
              parser.entries[1].state ==
                  HTTP_PARSER_STATE_REJECTED,
          "REQUEST_LINE rejects to REJECTED");

    headers = header_progress(4U, 5U);
    CHECK(http_parser_advance(
              &parser,
              &third,
              HTTP_PARSER_STATE_REQUEST_LINE,
              &request_line) == HTTP_PARSER_OK &&
              http_parser_advance(
                  &parser,
                  &third,
                  HTTP_PARSER_STATE_HEADERS,
                  &(struct http_parser_progress){
                      0U, 4U, 4U, 0U, 4U
                  }) == HTTP_PARSER_OK &&
              http_parser_advance(
                  &parser,
                  &third,
                  HTTP_PARSER_STATE_HEADERS,
                  &headers) == HTTP_PARSER_OK &&
              http_parser_reject(
                  &parser,
                  &third,
                  &headers) == HTTP_PARSER_OK &&
              parser.entries[2].state ==
                  HTTP_PARSER_STATE_REJECTED,
          "HEADERS rejects to REJECTED with exact progress");

    CHECK(http_parser_advance(
              &parser,
              &third,
              HTTP_PARSER_STATE_COMPLETE,
              &headers) == HTTP_PARSER_TERMINAL &&
              http_parser_reject(
                  &parser,
                  &third,
                  &headers) == HTTP_PARSER_TERMINAL &&
              parser.entries[2].state ==
                  HTTP_PARSER_STATE_REJECTED,
          "terminal REJECTED rejects further publication");
    CHECK(http_parser_state_at(&parser, 3U) ==
              HTTP_PARSER_STATE_EMPTY &&
              http_parser_generation_at(&parser, 3U) == 4U &&
              http_parser_active_count(&parser) ==
                  HTTP_PARSER_SLOT_COUNT,
          "rejected slots do not disturb untouched slot");

    for (index = 0U; index < HTTP_PARSER_SLOT_COUNT - 1U;
         ++index) {
        struct http_parser_token token = make_token(index, index + 1U);

        CHECK(http_parser_release(&parser, &token) ==
                  HTTP_PARSER_OK &&
                  parser.active_count ==
                      HTTP_PARSER_SLOT_COUNT - index - 1U,
              "released parser slot restores active count");
    }
    CHECK(http_parser_release(&parser, &fourth) == HTTP_PARSER_OK &&
              parser.active_count == 0U,
          "all parser slots release independently");
    CHECK(http_parser_progress_at(
              &parser, 0U, &progress) == HTTP_PARSER_OK &&
              progress_equal(&progress, &empty),
          "released parser slot exposes empty progress");
}

int main(void)
{
    test_strings_and_unpublished();
    test_init_start_release();
    test_exact_one_way_publication();
    test_incremental_and_invalid_progress();
    test_stale_generation_and_limits();
    test_rejection_and_slot_isolation();

    if (failures != 0) {
        fprintf(stderr, "HTTP parser state fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP parser state host tests: ALL OK\n");
    return 0;
}

#endif
