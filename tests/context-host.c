#include "context.h"
#include <stdint.h>
#include <stdio.h>

#ifdef HFOC_CONTEXT_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static uint8_t switch_stack_a[4096] __attribute__((aligned(16)));
static uint8_t switch_stack_b[4096] __attribute__((aligned(16)));
static struct context_frame current_frame;
static struct context_frame context_a;
static struct context_frame context_b;
static volatile uint64_t switch_order;

static void context_thread_a(void) {
    CHECK(switch_order == 1U, "thread A starts after main switch");
    switch_order = 2U;
    CHECK(context_switch(&context_a, &context_b) == CONTEXT_OK,
          "thread A resumes from thread B with CONTEXT_OK");
    CHECK(switch_order == 3U, "thread A resumes after thread B switch");
    switch_order = 4U;
    CHECK(context_switch(&context_a, &current_frame) == CONTEXT_OK,
          "thread A returns to main with CONTEXT_OK");
    CHECK(0, "thread A must not return after switching to main");
    for (;;) {
    }
}

static void context_thread_b(void) {
    CHECK(switch_order == 2U, "thread B starts after thread A switch");
    switch_order = 3U;
    CHECK(context_switch(&context_b, &context_a) == CONTEXT_OK,
          "thread B resumes thread A with CONTEXT_OK");
    CHECK(0, "thread B must not return after switching to thread A");
    for (;;) {
    }
}

static void test_frame_init(void) {
    const uintptr_t top = (uintptr_t)switch_stack_a + sizeof(switch_stack_a);
    struct context_frame frame;

    CHECK(context_frame_init(NULL, top, context_thread_a) == CONTEXT_NULL,
          "null frame returns CONTEXT_NULL");
    CHECK(context_frame_init(&frame, top, NULL) == CONTEXT_NULL,
          "null entry returns CONTEXT_NULL");
    CHECK(context_frame_init(&frame, 0U, context_thread_a) ==
              CONTEXT_BAD_STACK,
          "zero stack top returns CONTEXT_BAD_STACK");
    CHECK(context_frame_init(&frame, top - 8U, context_thread_a) ==
              CONTEXT_BAD_STACK,
          "unaligned stack top returns CONTEXT_BAD_STACK");
    CHECK(context_frame_init(&frame, top, context_thread_a) == CONTEXT_OK,
          "aligned stack top initializes context");
    CHECK(frame.rsp == top - 8U, "initial context rsp is stack top minus 8");
    CHECK(frame.rip == (uintptr_t)context_thread_a,
          "initial context rip is entry point");
    CHECK(frame.r15 == 0U && frame.r14 == 0U && frame.r13 == 0U &&
              frame.r12 == 0U && frame.rbx == 0U && frame.rbp == 0U,
          "initial callee-saved registers are zero");
}

static void test_invalid_next_does_not_change_current(void) {
    struct context_frame current = {
        .r15 = UINT64_C(0x1111111111111111),
        .r14 = UINT64_C(0x2222222222222222),
        .r13 = UINT64_C(0x3333333333333333),
        .r12 = UINT64_C(0x4444444444444444),
        .rbx = UINT64_C(0x5555555555555555),
        .rbp = UINT64_C(0x6666666666666666),
        .rip = UINT64_C(0x7777777777777777),
        .rsp = UINT64_C(0x8888888888888888),
    };
    struct context_frame null_stack = {0};
    struct context_frame noncanonical = {0};
    struct context_frame misaligned = {0};

    noncanonical.rsp = UINT64_C(0x0000800000001000);
    misaligned.rsp = UINT64_C(0xffff800000001001);

    CHECK(context_switch(NULL, &current) == CONTEXT_NULL,
          "null current returns CONTEXT_NULL");
    CHECK(context_switch(&current, NULL) == CONTEXT_NULL,
          "null next returns CONTEXT_NULL");
    CHECK(context_switch(&current, &null_stack) == CONTEXT_BAD_STACK,
          "null next stack returns CONTEXT_BAD_STACK");
    CHECK(context_switch(&current, &noncanonical) == CONTEXT_BAD_STACK,
          "noncanonical next stack returns CONTEXT_BAD_STACK");
    CHECK(context_switch(&current, &misaligned) == CONTEXT_MISALIGNED,
          "misaligned next stack returns CONTEXT_MISALIGNED");
    CHECK(current.rsp == UINT64_C(0x8888888888888888) &&
              current.rip == UINT64_C(0x7777777777777777) &&
              current.r15 == UINT64_C(0x1111111111111111) &&
              current.r14 == UINT64_C(0x2222222222222222) &&
              current.r13 == UINT64_C(0x3333333333333333) &&
              current.r12 == UINT64_C(0x4444444444444444) &&
              current.rbx == UINT64_C(0x5555555555555555) &&
              current.rbp == UINT64_C(0x6666666666666666),
          "invalid next leaves current context frame unchanged");
}

static void test_valid_round_trip(void) {
    const uintptr_t top_a =
        (uintptr_t)switch_stack_a + sizeof(switch_stack_a);
    const uintptr_t top_b =
        (uintptr_t)switch_stack_b + sizeof(switch_stack_b);

    current_frame = (struct context_frame){0};
    CHECK(context_frame_init(&context_a, top_a, context_thread_a) ==
              CONTEXT_OK,
          "context A frame initializes");
    CHECK(context_frame_init(&context_b, top_b, context_thread_b) ==
              CONTEXT_OK,
          "context B frame initializes");

    switch_order = 1U;
    CHECK(context_switch(&current_frame, &context_a) == CONTEXT_OK,
          "main switches to context A and resumes with CONTEXT_OK");
    CHECK(switch_order == 4U, "thread A and thread B completed round-trip");
}

int main(void) {
    test_frame_init();
    test_invalid_next_does_not_change_current();
    test_valid_round_trip();

    if (failures != 0) {
        fprintf(stderr, "Context host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Context host tests: ALL OK");
    return 0;
}

#endif
