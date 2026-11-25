/*
 * ITEM-629: bounded TTY queues, EOF/hangup and terminal geometry.
 * The blocking descriptor path is proven by the ring3 SSH shell proof; this
 * host model drives the same production tty.c without the scheduler.
 */
#include "tty.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

static void test_capacity_and_eof(void)
{
    struct tty *tty = NULL;
    unsigned char input[HFOC_TTY_INPUT_MAX];
    unsigned char output[HFOC_TTY_INPUT_MAX];
    size_t done = 0U;
    size_t accepted = 0U;

    CHECK(tty_create(&tty) == TTY_OK && tty != NULL,
          "a terminal is created");
    memset(input, 'a', sizeof(input));
    CHECK(tty_input(tty, input, sizeof(input), &accepted) == TTY_OK &&
              accepted == sizeof(input) &&
              tty_input_pending(tty) == HFOC_TTY_INPUT_MAX,
          "the input queue accepts exactly its bound");
    CHECK(tty_input(tty, input, 1U, &accepted) == TTY_FULL &&
              accepted == 0U,
          "an input overrun is refused without losing the queue");
    CHECK(tty_read_now(tty, output, 100U, &done) == TTY_OK &&
              done == 100U && output[0] == 'a',
          "a partial read returns the queued prefix");
    tty_input_eof(tty);
    CHECK(tty_input(tty, input, 1U, &accepted) == TTY_HUNGUP,
          "input after EOF is refused");
    CHECK(tty_read_now(tty, output, sizeof(output), &done) == TTY_OK &&
              done == HFOC_TTY_INPUT_MAX - 100U,
          "the queued bytes drain after EOF");
    CHECK(tty_read_now(tty, output, 1U, &done) == TTY_OK && done == 0U,
          "draining an EOF terminal is a clean EOF");
    tty_destroy(tty);
}

static void test_output_and_hangup(void)
{
    struct tty *tty = NULL;
    unsigned char bytes[HFOC_TTY_OUTPUT_MAX + 1U];
    unsigned char out[HFOC_TTY_OUTPUT_MAX];
    size_t done = 0U;

    CHECK(tty_create(&tty) == TTY_OK, "an output terminal is created");
    memset(bytes, 'z', sizeof(bytes));
    CHECK(tty_write_now(tty, bytes, sizeof(bytes), &done) == TTY_OK &&
              done == HFOC_TTY_OUTPUT_MAX &&
              tty_output_pending(tty) == HFOC_TTY_OUTPUT_MAX,
          "the output queue accepts its bound");
    CHECK(tty_write_now(tty, bytes, 1U, &done) == TTY_AGAIN && done == 0U,
          "a full output queue reports backpressure");
    CHECK(tty_output_take(tty, out, 50U, &done) == TTY_OK && done == 50U &&
              out[0] == 'z',
          "the SSH drain takes a bounded prefix");
    tty_hangup(tty);
    CHECK(tty_hung_up(tty), "hangup is visible");
    CHECK(tty_read_now(tty, out, 1U, &done) == TTY_OK && done == 0U,
          "a hung-up terminal reads EOF");
    CHECK(tty_write_now(tty, bytes, 1U, &done) == TTY_HUNGUP,
          "a hung-up terminal refuses output");
    tty_destroy(tty);
}

static void test_geometry_and_slots(void)
{
    struct tty *ttys[HFOC_TTY_MAX];
    struct tty *extra = NULL;
    uint32_t index;

    for (index = 0U; index < HFOC_TTY_MAX; ++index) {
        CHECK(tty_create(&ttys[index]) == TTY_OK, "a terminal slot exists");
    }
    CHECK(tty_create(&extra) == TTY_NO_SPACE,
          "the third session terminal is refused");
    tty_resize(ttys[0], 132U, 43U);
    CHECK(tty_columns(ttys[0]) == 132U && tty_rows(ttys[0]) == 43U,
          "window-change geometry is published");
    tty_resize(ttys[0], 0U, 0U);
    CHECK(tty_columns(ttys[0]) == 80U && tty_rows(ttys[0]) == 24U,
          "invalid geometry falls back to 80x24");
    for (index = 0U; index < HFOC_TTY_MAX; ++index) {
        tty_destroy(ttys[index]);
    }
    CHECK(tty_create(&extra) == TTY_OK,
          "destroying the creator reference releases a slot");
    tty_destroy(extra);
}

int main(void)
{
    test_capacity_and_eof();
    test_output_and_hangup();
    test_geometry_and_slots();
    if (failures != 0) {
        fprintf(stderr, "tty host test: %d FAILURES\n", failures);
        return 1;
    }
    printf("tty host test: ALL OK\n");
    return 0;
}
