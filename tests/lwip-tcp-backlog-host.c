#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_backlog.h"

#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_BACKLOG_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

u32_t sys_now(void)
{
    return 1000U;
}

static struct tcp_pcb *make_pcb(void)
{
    struct tcp_pcb *pcb = tcp_new();

    if (pcb != NULL) {
        pcb->state = ESTABLISHED;
        pcb->listener = NULL;
    }
    return pcb;
}

static void discard_pcb(struct tcp_pcb *pcb)
{
    if (pcb != NULL) {
        tcp_abort(pcb);
    }
}

static bool init_backlog(
    struct hfoc_lwip_tcp_backlog *backlog)
{
    memset(backlog, 0, sizeof(*backlog));
    return hfoc_lwip_tcp_backlog_init(backlog) ==
        HFOC_LWIP_TCP_BACKLOG_OK;
}

static void test_strings(void)
{
    CHECK(hfoc_lwip_tcp_backlog_state_string(
              HFOC_LWIP_TCP_BACKLOG_STATE_FREE) != NULL &&
              hfoc_lwip_tcp_backlog_state_string(
                  HFOC_LWIP_TCP_BACKLOG_STATE_PENDING) != NULL &&
              hfoc_lwip_tcp_backlog_state_string(
                  (enum hfoc_lwip_tcp_backlog_state)99U) == NULL,
          "backlog state strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_backlog_status_string(
              HFOC_LWIP_TCP_BACKLOG_OK) != NULL &&
              hfoc_lwip_tcp_backlog_status_string(
                  HFOC_LWIP_TCP_BACKLOG_NULL) != NULL &&
              hfoc_lwip_tcp_backlog_status_string(
                  HFOC_LWIP_TCP_BACKLOG_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_backlog_status_string(
                  HFOC_LWIP_TCP_BACKLOG_FULL) != NULL &&
              hfoc_lwip_tcp_backlog_status_string(
                  HFOC_LWIP_TCP_BACKLOG_EMPTY) != NULL &&
              hfoc_lwip_tcp_backlog_status_string(
                  HFOC_LWIP_TCP_BACKLOG_INVALID_PCB) != NULL &&
              hfoc_lwip_tcp_backlog_status_string(
                  HFOC_LWIP_TCP_BACKLOG_DUPLICATE_PCB) != NULL &&
              hfoc_lwip_tcp_backlog_status_string(
                  HFOC_LWIP_TCP_BACKLOG_GENERATION_EXHAUSTED) != NULL &&
              hfoc_lwip_tcp_backlog_status_string(
                  (enum hfoc_lwip_tcp_backlog_status)99U) == NULL,
          "backlog status strings cover known and unknown values");
}

static void test_init_and_accessors(void)
{
    struct hfoc_lwip_tcp_backlog backlog;
    uint32_t index;

    memset(&backlog, 0, sizeof(backlog));
    CHECK(hfoc_lwip_tcp_backlog_init(NULL) ==
              HFOC_LWIP_TCP_BACKLOG_NULL,
          "backlog init rejects null backlog");
    CHECK(!hfoc_lwip_tcp_backlog_published(&backlog) &&
              hfoc_lwip_tcp_backlog_capacity(&backlog) == 0U &&
              hfoc_lwip_tcp_backlog_pending_count(&backlog) == 0U,
          "unpublished backlog accessors fail closed");

    CHECK(init_backlog(&backlog),
          "valid backlog config publishes fixed FIFO");
    CHECK(hfoc_lwip_tcp_backlog_init(&backlog) ==
              HFOC_LWIP_TCP_BACKLOG_BAD_STATE,
          "published backlog rejects double init");
    CHECK(hfoc_lwip_tcp_backlog_published(&backlog) &&
              backlog.version == HFOC_LWIP_TCP_BACKLOG_VERSION &&
              backlog.capacity == HFOC_LWIP_TCP_BACKLOG_CAPACITY &&
              backlog.pending_count == 0U &&
              backlog.head == 0U &&
              backlog.tail == 0U &&
              hfoc_lwip_tcp_backlog_capacity(&backlog) ==
                  HFOC_LWIP_TCP_BACKLOG_CAPACITY &&
              hfoc_lwip_tcp_backlog_pending_count(&backlog) == 0U,
          "published backlog exposes exact empty FIFO state");
    for (index = 0U; index < HFOC_LWIP_TCP_BACKLOG_CAPACITY;
         ++index) {
        CHECK(backlog.entries[index].state ==
                  HFOC_LWIP_TCP_BACKLOG_STATE_FREE &&
                  backlog.entries[index].generation ==
                      HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN &&
                  backlog.entries[index].pcb == NULL,
              "published backlog seeds free generation-one entries");
    }
}

static void test_admit_validation(void)
{
    struct hfoc_lwip_tcp_backlog backlog;
    struct hfoc_lwip_tcp_backlog snapshot;
    struct hfoc_lwip_tcp_backlog_token token;
    struct tcp_pcb *pcb;
    struct tcp_pcb *listen_pcb;

    CHECK(init_backlog(&backlog),
          "admit-validation fixture initializes backlog");
    pcb = make_pcb();
    CHECK(pcb != NULL,
          "admit-validation fixture allocates established PCB");

    token.index = 99U;
    token.generation = 99U;
    CHECK(hfoc_lwip_tcp_backlog_admit(
              &backlog, NULL, &token) ==
              HFOC_LWIP_TCP_BACKLOG_NULL &&
              token.index == 99U &&
              token.generation == 99U,
          "admit rejects null PCB without token mutation");
    CHECK(hfoc_lwip_tcp_backlog_admit(
              &backlog, pcb, NULL) ==
              HFOC_LWIP_TCP_BACKLOG_NULL,
          "admit rejects null token");

    listen_pcb = make_pcb();
    CHECK(listen_pcb != NULL,
          "admit-validation fixture allocates invalid PCB");
    listen_pcb->state = LISTEN;
    memcpy(&snapshot, &backlog, sizeof(backlog));
    CHECK(hfoc_lwip_tcp_backlog_admit(
              &backlog, listen_pcb, &token) ==
              HFOC_LWIP_TCP_BACKLOG_INVALID_PCB &&
              memcmp(&backlog, &snapshot, sizeof(backlog)) == 0,
          "admit rejects LISTEN PCB without mutation");
    discard_pcb(listen_pcb);

    CHECK(hfoc_lwip_tcp_backlog_admit(
              &backlog, pcb, &token) ==
              HFOC_LWIP_TCP_BACKLOG_OK &&
              token.index == 0U &&
              token.generation ==
                  HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN &&
              backlog.pending_count == 1U &&
              backlog.entries[token.index].state ==
                  HFOC_LWIP_TCP_BACKLOG_STATE_PENDING &&
              backlog.entries[token.index].pcb == pcb,
          "admit stores one generation-tagged pending PCB");
    memcpy(&snapshot, &backlog, sizeof(backlog));
    CHECK(hfoc_lwip_tcp_backlog_admit(
              &backlog, pcb, &token) ==
              HFOC_LWIP_TCP_BACKLOG_DUPLICATE_PCB &&
              memcmp(&backlog, &snapshot, sizeof(backlog)) == 0,
          "admit rejects duplicate pending PCB without mutation");

    CHECK(hfoc_lwip_tcp_backlog_drain(
              &backlog, &token, &pcb) ==
              HFOC_LWIP_TCP_BACKLOG_OK &&
              backlog.pending_count == 0U,
          "admit-validation fixture drains pending PCB");
    discard_pcb(pcb);
}

static void test_full_denial_and_drain_recovery(void)
{
    struct hfoc_lwip_tcp_backlog backlog;
    struct hfoc_lwip_tcp_backlog snapshot;
    struct hfoc_lwip_tcp_backlog_token tokens[4];
    struct hfoc_lwip_tcp_backlog_token token;
    struct tcp_pcb *pcbs[4];
    struct tcp_pcb *pcb;
    uint32_t index;

    CHECK(init_backlog(&backlog),
          "full-denial fixture initializes backlog");
    for (index = 0U; index < HFOC_LWIP_TCP_BACKLOG_CAPACITY;
         ++index) {
        pcbs[index] = make_pcb();
        CHECK(pcbs[index] != NULL &&
                  hfoc_lwip_tcp_backlog_admit(
                      &backlog, pcbs[index], &tokens[index]) ==
                      HFOC_LWIP_TCP_BACKLOG_OK &&
                  tokens[index].index == index &&
                  tokens[index].generation ==
                      HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN &&
                  backlog.pending_count == index + 1U,
              "four admits fill FIFO in order with exact counts");
    }

    pcb = make_pcb();
    CHECK(pcb != NULL,
          "full-denial fixture allocates overflow PCB");
    token.index = 99U;
    token.generation = 99U;
    memcpy(&snapshot, &backlog, sizeof(backlog));
    CHECK(hfoc_lwip_tcp_backlog_admit(
              &backlog, pcb, &token) ==
              HFOC_LWIP_TCP_BACKLOG_FULL &&
              token.index == 99U &&
              token.generation == 99U &&
              memcmp(&backlog, &snapshot, sizeof(backlog)) == 0,
          "fifth admit rejects overflow without mutating FIFO");
    discard_pcb(pcb);

    CHECK(hfoc_lwip_tcp_backlog_drain(
              &backlog, &token, &pcb) ==
              HFOC_LWIP_TCP_BACKLOG_OK &&
              token.index == 0U &&
              token.generation ==
                  HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN &&
              pcb == pcbs[0] &&
              backlog.pending_count ==
                  HFOC_LWIP_TCP_BACKLOG_CAPACITY - 1U,
          "drain returns oldest pending entry and restores capacity");
    discard_pcb(pcb);

    pcb = make_pcb();
    CHECK(pcb != NULL,
          "recovery fixture allocates replacement PCB");
    CHECK(hfoc_lwip_tcp_backlog_admit(
              &backlog, pcb, &token) ==
              HFOC_LWIP_TCP_BACKLOG_OK &&
              token.index == 0U &&
              token.generation ==
                  HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN + 1U &&
              backlog.pending_count ==
                  HFOC_LWIP_TCP_BACKLOG_CAPACITY,
          "after one drain a new admit succeeds with incremented generation");
    pcbs[0] = pcb;

    for (index = 0U; index < HFOC_LWIP_TCP_BACKLOG_CAPACITY;
         ++index) {
        uint32_t expected_index = (index + 1U) %
            HFOC_LWIP_TCP_BACKLOG_CAPACITY;

        CHECK(hfoc_lwip_tcp_backlog_drain(
                  &backlog, &token, &pcb) ==
                  HFOC_LWIP_TCP_BACKLOG_OK &&
                  pcb == pcbs[expected_index] &&
                  backlog.pending_count ==
                      HFOC_LWIP_TCP_BACKLOG_CAPACITY - index - 1U,
              "drain preserves FIFO order across slot reuse");
        discard_pcb(pcb);
    }
}

static void test_wraparound_and_failed_accept_drain(void)
{
    struct hfoc_lwip_tcp_backlog backlog;
    struct hfoc_lwip_tcp_backlog snapshot;
    struct hfoc_lwip_tcp_backlog_token token;
    struct tcp_pcb *pcb;
    uint32_t cycle;

    CHECK(init_backlog(&backlog),
          "wraparound fixture initializes backlog");
    for (cycle = 0U;
         cycle < HFOC_LWIP_TCP_BACKLOG_CAPACITY * 2U;
         ++cycle) {
        pcb = make_pcb();
        CHECK(pcb != NULL &&
                  hfoc_lwip_tcp_backlog_admit(
                      &backlog, pcb, &token) ==
                      HFOC_LWIP_TCP_BACKLOG_OK &&
                  token.index ==
                      cycle % HFOC_LWIP_TCP_BACKLOG_CAPACITY &&
                  token.generation ==
                      HFOC_LWIP_TCP_BACKLOG_GENERATION_MIN +
                      cycle / HFOC_LWIP_TCP_BACKLOG_CAPACITY &&
                  hfoc_lwip_tcp_backlog_drain(
                      &backlog, &token, &pcb) ==
                      HFOC_LWIP_TCP_BACKLOG_OK &&
                  backlog.pending_count == 0U,
              "FIFO wraparound reuses drained slot with new generation");
        discard_pcb(pcb);
    }

    CHECK(hfoc_lwip_tcp_backlog_drain(
              &backlog, &token, &pcb) ==
              HFOC_LWIP_TCP_BACKLOG_EMPTY,
          "drain on empty FIFO returns empty");
    memcpy(&snapshot, &backlog, sizeof(backlog));
    CHECK(hfoc_lwip_tcp_backlog_drain(
              &backlog, &token, &pcb) ==
              HFOC_LWIP_TCP_BACKLOG_EMPTY &&
              memcmp(&backlog, &snapshot, sizeof(backlog)) == 0,
          "drain on empty FIFO does not mutate state");

    pcb = make_pcb();
    CHECK(pcb != NULL &&
              hfoc_lwip_tcp_backlog_admit(
                  &backlog, pcb, &token) ==
                  HFOC_LWIP_TCP_BACKLOG_OK &&
              hfoc_lwip_tcp_backlog_drain(
                  &backlog, &token, &pcb) ==
                  HFOC_LWIP_TCP_BACKLOG_OK &&
              backlog.pending_count == 0U,
          "failed-accept drain removes pending PCB before caller abort");
    discard_pcb(pcb);
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "backlog fixture starts fixed lwIP allocator");

    test_strings();
    test_init_and_accessors();
    test_admit_validation();
    test_full_denial_and_drain_recovery();
    test_wraparound_and_failed_accept_drain();

    if (failures != 0) {
        fprintf(stderr, "lwIP TCP backlog fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("lwIP TCP backlog host tests: ALL OK\n");
    return 0;
}

#endif
