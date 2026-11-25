/*
 * ITEM-627: the bounded pipe ring, EOF, broken pipe and end lifetime.
 * The host proof drives the production pipe.c with the wait path compiled out;
 * the blocking round trip is proven by the ring3 proof.
 */
#include "pipe.h"

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

static void test_capacity_and_ring(void)
{
    struct hfoc_pipe *pipe = NULL;
    unsigned char out[HFOC_PIPE_CAPACITY];
    unsigned char in[HFOC_PIPE_CAPACITY];
    size_t done = 0U;

    CHECK(pipe_create(&pipe) == PIPE_OK && pipe != NULL,
          "a pipe is created");
    memset(in, 'A', sizeof(in));
    CHECK(pipe_write_now(pipe, in, sizeof(in), &done) == PIPE_OK &&
              done == sizeof(in) && pipe_pending(pipe) == HFOC_PIPE_CAPACITY,
          "the ring accepts exactly its capacity");
    CHECK(pipe_write_now(pipe, in, 1U, &done) == PIPE_WOULD_BLOCK &&
              done == 0U,
          "a full ring reports backpressure without losing bytes");
    CHECK(pipe_read_now(pipe, out, 100U, &done) == PIPE_OK && done == 100U &&
              out[0] == 'A' && out[99] == 'A',
          "a partial read returns the requested prefix");
    CHECK(pipe_write_now(pipe, in, 100U, &done) == PIPE_OK && done == 100U,
          "the freed space accepts a later write");
    CHECK(pipe_read_now(pipe, out, sizeof(out), &done) == PIPE_OK &&
              done == sizeof(out) && pipe_pending(pipe) == 0U,
          "the whole ring drains without wrap loss");
    pipe_end_release(&pipe->read_end);
    pipe_end_release(&pipe->write_end);
}

static void test_eof_and_broken(void)
{
    struct hfoc_pipe *pipe = NULL;
    unsigned char byte = 'x';
    size_t done = 0U;

    CHECK(pipe_create(&pipe) == PIPE_OK, "EOF pipe is created");
    pipe_end_release(&pipe->write_end);
    CHECK(pipe_read_now(pipe, &byte, 1U, &done) == PIPE_OK && done == 0U,
          "a reader sees EOF when every writer closed");
    pipe_end_release(&pipe->read_end);

    CHECK(pipe_create(&pipe) == PIPE_OK, "broken-pipe pipe is created");
    pipe_end_release(&pipe->read_end);
    CHECK(pipe_write_now(pipe, &byte, 1U, &done) == PIPE_BROKEN && done == 0U,
          "a writer sees EPIPE when every reader closed");
    pipe_end_release(&pipe->write_end);
}

static void test_dup_retain(void)
{
    struct hfoc_pipe *pipe = NULL;
    unsigned char byte = 'q';
    size_t done = 0U;

    CHECK(pipe_create(&pipe) == PIPE_OK, "dup pipe is created");
    pipe_end_retain(&pipe->write_end);
    pipe_end_release(&pipe->write_end);
    CHECK(pipe_write_now(pipe, &byte, 1U, &done) == PIPE_OK && done == 1U,
          "a retained end keeps the direction open");
    pipe_end_release(&pipe->write_end);
    CHECK(pipe_write_now(pipe, &byte, 1U, &done) != PIPE_OK,
          "the last release closes the write direction");
    pipe_end_release(&pipe->read_end);
}

static void test_slots(void)
{
    struct hfoc_pipe *pipes[HFOC_PIPE_MAX];
    struct hfoc_pipe *extra = NULL;
    uint32_t index;

    for (index = 0U; index < HFOC_PIPE_MAX; ++index) {
        CHECK(pipe_create(&pipes[index]) == PIPE_OK,
              "a pipe slot is available");
    }
    CHECK(pipe_create(&extra) == PIPE_NO_SPACE,
          "the ninth pipe is refused");
    for (index = 0U; index < HFOC_PIPE_MAX; ++index) {
        pipe_end_release(&pipes[index]->read_end);
        pipe_end_release(&pipes[index]->write_end);
    }
    CHECK(pipe_create(&extra) == PIPE_OK,
          "closing both ends releases the slot");
    pipe_end_release(&extra->read_end);
    pipe_end_release(&extra->write_end);
}

int main(void)
{
    test_capacity_and_ring();
    test_eof_and_broken();
    test_dup_retain();
    test_slots();
    if (failures != 0) {
        fprintf(stderr, "pipe host test: %d FAILURES\n", failures);
        return 1;
    }
    printf("pipe host test: ALL OK\n");
    return 0;
}
