/*
 * ITEM-521 (increment 1): host proof for the placement-independent journal
 * core.
 *
 * Drives install_journal_core.c over a RAM backend that stands in for any of
 * the three placements, and pins the properties the wiring plan asks for:
 * the pre-bind staging ring and its order, the bounded ring and the named
 * overflow, the named write refusal that does not fail the run, the named
 * FULL, the re-entrancy guard, the truncation notice, the header of a fresh
 * journal, the absence of a second header in a resumed one, and the capacity
 * bound every write path obeys (increment 9).
 */

#include "install_journal_core.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
static unsigned checks;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

#define SLOT_CAPACITY 128U

struct ram_backend {
    uint8_t slots[SLOT_CAPACITY][INSTALL_JOURNAL_SLOT_BYTES];
    uint32_t capacity;
    uint32_t committed;
    uint32_t flushes;
    uint32_t writes;   /* every slot the core handed to this placement */
    uint32_t overruns; /* handovers past `capacity`: the placement refused */
    uint32_t fail_after; /* refuse every handover from this one on; 0 = never */
    bool fail_write;
    bool fail_flush;
    bool reenter;
    struct install_journal *journal;
};

static uint32_t be_committed(void *context)
{
    struct ram_backend *backend = context;
    return backend->committed;
}

static uint32_t be_capacity(void *context)
{
    struct ram_backend *backend = context;
    return backend->capacity;
}

static bool be_write(void *context,
                     const uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES])
{
    struct ram_backend *backend = context;
    if (backend->reenter && backend->journal != NULL) {
        /* The funnel is re-entered from inside a backend write: the core must
           ignore this record instead of recursing. */
        install_journal_observe(backend->journal, "reentrant", 9U);
    }
    backend->writes += 1U;
    /*
     * ITEM-521 (increment 9): the placement holds `capacity` slots, header
     * included, and it is the only one that can refuse a handover - so a core
     * that asks for one more is asking for a sector outside the region.  The
     * fixture counts that instead of hiding it, which is how the witness sees
     * a write path that ignores the declared bound.
     */
    if (backend->capacity != 0U && backend->committed >= backend->capacity) {
        backend->overruns += 1U;
        return false;
    }
    if (backend->fail_write || backend->committed >= SLOT_CAPACITY) {
        return false;
    }
    if (backend->fail_after != 0U && backend->writes >= backend->fail_after) {
        return false;
    }
    memcpy(backend->slots[backend->committed], slot,
           INSTALL_JOURNAL_SLOT_BYTES);
    backend->committed += 1U;
    return true;
}

static bool be_flush(void *context)
{
    struct ram_backend *backend = context;
    if (backend->fail_flush) {
        return false;
    }
    backend->flushes += 1U;
    return true;
}

static struct install_journal_backend backend_of(struct ram_backend *ram)
{
    struct install_journal_backend backend = {
        .name = "ram",
        .context = ram,
        .committed_slots = be_committed,
        .capacity_slots = be_capacity,
        .write_slot = be_write,
        .flush = be_flush,
    };
    return backend;
}

struct notify_log {
    char lines[8][160];
    uint32_t count;
};

static void notify_cb(void *context, const char *text)
{
    struct notify_log *log = context;
    if (log->count < 8U) {
        size_t length = strlen(text);
        if (length >= sizeof(log->lines[0])) {
            length = sizeof(log->lines[0]) - 1U;
        }
        memcpy(log->lines[log->count], text, length);
        log->lines[log->count][length] = '\0';
        log->count += 1U;
    }
}

static uint32_t le32_at(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Independent decode of a record slot: returns false when the length or the
   CRC does not check out (a torn or foreign slot must not be printed). */
static bool decode_record(const uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES],
                          char *text, size_t text_size, size_t *out_length)
{
    uint32_t length = le32_at(&slot[0]);
    if (length == 0U || length > INSTALL_JOURNAL_TEXT_MAX ||
        (size_t)length + 8U > INSTALL_JOURNAL_SLOT_BYTES) {
        return false;
    }
    uint32_t crc = le32_at(&slot[4]);
    if (crc != install_journal_crc32(&slot[8], length)) {
        return false;
    }
    size_t take = length < text_size - 1U ? length : text_size - 1U;
    memcpy(text, &slot[8], take);
    text[take] = '\0';
    *out_length = length;
    return true;
}

static const char *record_text(struct ram_backend *ram, uint32_t index,
                               char *buffer, size_t buffer_size)
{
    if (index >= ram->committed) {
        return NULL;
    }
    size_t length = 0U;
    if (!decode_record(ram->slots[index], buffer, buffer_size, &length)) {
        return NULL;
    }
    return buffer;
}

static void test_pending_replay_in_order(void)
{
    struct ram_backend ram = {.capacity = 32U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);

    install_journal_observe(&journal, "[BOOT] machine cpus=1 memory=512M", 33U);
    install_journal_observe(&journal, "[STORAGE-REPORT] found=2 accepted=2", 35U);
    CHECK(journal.state == INSTALL_JOURNAL_UNBOUND, "records stage before bind");
    CHECK(journal.pending_count == 2U, "two records wait in the ring");
    CHECK(journal.written == 0U, "nothing was written before bind");

    CHECK(install_journal_bind(&journal, &backend),
          "bind with pending records succeeds");
    CHECK(journal.state == INSTALL_JOURNAL_READY, "state is READY after bind");
    CHECK(ram.committed == 3U, "header plus two replayed records");
    CHECK(install_journal_state_string(journal.state)[0] == 'R',
          "READY state token");
    CHECK(journal.pending_count == 0U, "the ring is drained");
    CHECK(journal.written == 3U, "written counts the header");

    char buffer[INSTALL_JOURNAL_SLOT_BYTES];
    CHECK(record_text(&ram, 1U, buffer, sizeof(buffer)) != NULL &&
              strncmp(buffer, "[BOOT] machine", 14U) == 0,
          "the oldest staged record is replayed first");
    CHECK(record_text(&ram, 2U, buffer, sizeof(buffer)) != NULL &&
              strncmp(buffer, "[STORAGE-REPORT]", 16U) == 0,
          "the ring keeps publication order");

    install_journal_observe(&journal, "[PRODUCTION-INSTALL] refused reason=NO_DISK",
                            38U);
    CHECK(ram.committed == 4U, "a bound record goes straight to the backend");
}

static void test_ring_bound_and_overflow_named(void)
{
    struct ram_backend ram = {.capacity = 256U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);

    char line[64];
    for (uint32_t i = 0U; i < INSTALL_JOURNAL_PENDING_CAPACITY + 6U; ++i) {
        int length = snprintf(line, sizeof(line), "[EVENT] index=%u", i);
        install_journal_observe(&journal, line, (size_t)length);
    }
    CHECK(journal.pending_count == INSTALL_JOURNAL_PENDING_CAPACITY,
          "the ring holds exactly its bound");
    CHECK(journal.dropped == 6U, "six oldest records were dropped and counted");

    CHECK(install_journal_bind(&journal, &backend), "bind replays the ring");
    char buffer[INSTALL_JOURNAL_SLOT_BYTES];
    CHECK(record_text(&ram, 1U, buffer, sizeof(buffer)) != NULL &&
              strncmp(buffer, "[INSTALL-LOG] state=OVERFLOW dropped=6", 37U) ==
                  0,
          "the loss is named before anything else");
    CHECK(record_text(&ram, 2U, buffer, sizeof(buffer)) != NULL &&
              strcmp(buffer, "[EVENT] index=6") == 0,
          "the kept records start at the first one that fits");
    CHECK(record_text(&ram, 1U + INSTALL_JOURNAL_PENDING_CAPACITY, buffer,
                      sizeof(buffer)) != NULL &&
              strcmp(buffer, "[EVENT] index=69") == 0,
          "the newest staged record is replayed last");
}

static void test_write_failure_is_named_and_silent(void)
{
    struct ram_backend ram = {.capacity = 32U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);
    CHECK(install_journal_bind(&journal, &backend), "bind on a healthy backend");

    ram.fail_write = true;
    install_journal_observe(&journal, "[PRODUCTION-INSTALL] refused", 29U);
    CHECK(journal.state == INSTALL_JOURNAL_UNAVAILABLE,
          "a refused write turns the journal off, not the run");
    CHECK(log.count == 1U &&
              strcmp(log.lines[0],
                     "[INSTALL-LOG] state=UNAVAILABLE reason=WRITE") == 0,
          "the refusal is named once on the notify side (F7)");
    install_journal_observe(&journal, "[PRODUCTION-INSTALL] refused", 29U);
    install_journal_observe(&journal, "[PRODUCTION-INSTALL] refused", 29U);
    CHECK(log.count == 1U, "the refusal is not repeated");
    CHECK(journal.refused == 2U, "later records are counted, not written");
}

/*
 * ITEM-521 (requirement 3, F-B of item-521-wiring-audit-2026-10-12.md): a
 * medium that cannot carry the journal at all - an optical boot, a medium
 * declared read-only - is refused before any write is attempted.  The run is
 * not failed, the refusal is named once on the notify side, and no slot ever
 * reaches a placement.  A journal that is already bound is not the subject of
 * this decision and keeps running.
 */
static void test_placement_refusal_before_write_is_named(void)
{
    struct ram_backend ram = {.capacity = 32U};
    struct notify_log log = {0};
    struct install_journal journal;
    struct ram_backend running_ram = {.capacity = 32U};
    struct notify_log running_log = {0};
    struct install_journal running;
    struct install_journal_backend backend;
    struct install_journal_backend running_backend;

    install_journal_init(&journal, notify_cb, &log);
    backend = backend_of(&ram);
    install_journal_observe(&journal, "[BOOT] machine cpus=1 memory=512M", 33U);
    CHECK(!install_journal_refuse(&journal, "READ_ONLY"),
          "a placement that cannot carry the journal binds nothing");
    CHECK(journal.state == INSTALL_JOURNAL_UNAVAILABLE,
          "the read-only medium turns the journal off, not the run");
    CHECK(log.count == 1U &&
              strcmp(log.lines[0],
                     "[INSTALL-LOG] state=UNAVAILABLE reason=READ_ONLY") == 0,
          "the read-only medium is named once on the notify side");
    CHECK(!install_journal_refuse(&journal, "READ_ONLY"),
          "a second refusal binds nothing either");
    CHECK(log.count == 1U, "the refusal is not repeated");
    install_journal_observe(&journal, "[PRODUCTION-INSTALL] refused", 29U);
    CHECK(journal.refused == 1U,
          "records after the refusal are counted, not written");
    CHECK(ram.committed == 0U, "no slot reaches a placement that refused");

    install_journal_init(&running, notify_cb, &running_log);
    running_backend = backend_of(&running_ram);
    CHECK(install_journal_bind(&running, &running_backend),
          "a running journal binds");
    CHECK(!install_journal_refuse(&running, "READ_ONLY"),
          "a late refusal binds nothing");
    CHECK(running.state == INSTALL_JOURNAL_READY,
          "a running journal is not stopped by a late refusal");
    CHECK(running_log.count == 0U, "and nothing is named");
}

static void test_full_is_named_once(void)
{
    struct ram_backend ram = {.capacity = 3U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);
    CHECK(install_journal_bind(&journal, &backend), "bind with room for two");

    install_journal_observe(&journal, "first", 5U);
    CHECK(journal.state == INSTALL_JOURNAL_READY,
          "the header, one record and the end marker fit");
    CHECK(ram.committed == 2U, "the header and one record are on the medium");
    install_journal_observe(&journal, "second", 6U);
    CHECK(journal.state == INSTALL_JOURNAL_FULL,
          "the last usable slot ends in a named FULL");
    CHECK(log.count == 1U &&
              strcmp(log.lines[0],
                     "[INSTALL-LOG] state=FULL reason=CAPACITY") == 0,
          "FULL is named on the notify side");
    char marker[INSTALL_JOURNAL_SLOT_BYTES];
    CHECK(ram.committed == 3U, "the end marker takes the reserved slot");
    CHECK(record_text(&ram, 2U, marker, sizeof(marker)) != NULL &&
              strncmp(marker, "[INSTALL-LOG] state=FULL", 24U) == 0,
          "the end of the journal is named on the medium itself");
    install_journal_observe(&journal, "third", 5U);
    CHECK(journal.refused == 2U, "later records are refused and counted");
    CHECK(log.count == 1U, "FULL is not repeated");
}

/*
 * ITEM-521 (increment 9, fork-independent): the placement declares how many
 * slots it can hold, header included, and the core never hands it a slot past
 * that bound - in any write path.  Before this increment the replay at bind
 * (and the panic replay) wrote until the placement itself refused, which
 * handed the region a slot outside its end, named the refusal
 * `UNAVAILABLE reason=WRITE` instead of `FULL reason=CAPACITY`, and left the
 * staged records that no longer fitted uncounted.
 */
static void test_capacity_bounds_every_write_path(void)
{
    /* A fresh bind with more staged records than the placement can hold: the
       replay stops at the capacity, names FULL, and the rest is counted. */
    struct ram_backend ram = {.capacity = 8U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);
    for (uint32_t i = 0U; i < INSTALL_JOURNAL_PENDING_CAPACITY; ++i) {
        char line[48];
        int length = snprintf(line, sizeof(line), "[EVENT] staged=%u", i);
        install_journal_observe(&journal, line, (size_t)length);
    }
    CHECK(!install_journal_bind(&journal, &backend),
          "a bind whose records do not all fit is not reported ready");
    CHECK(ram.overruns == 0U,
          "no write path hands the placement a slot past its capacity");
    CHECK(ram.writes == 8U && ram.committed == 8U,
          "the journal stops exactly at the capacity it was given");
    CHECK(journal.state == INSTALL_JOURNAL_FULL,
          "no room ends in FULL, not in a write failure");
    CHECK(log.count == 1U &&
              strcmp(log.lines[0],
                     "[INSTALL-LOG] state=FULL reason=CAPACITY") == 0,
          "the reason is CAPACITY, not WRITE");
    char marker[INSTALL_JOURNAL_SLOT_BYTES];
    CHECK(record_text(&ram, 7U, marker, sizeof(marker)) != NULL &&
              strncmp(marker, "[INSTALL-LOG] state=FULL", 24U) == 0,
          "the end of the journal is named on the medium, in its last slot");
    CHECK(journal.refused ==
              INSTALL_JOURNAL_PENDING_CAPACITY - 6U,
          "the staged records that did not fit are counted, not forgotten");

    /* A resumed journal whose capacity is already used writes nothing at all:
       there is no room even for the end marker, so the naming is the notify
       side's job (requirement 5 stays honest either way). */
    struct ram_backend full_ram = {.capacity = 4U, .committed = 4U};
    struct notify_log full_log = {0};
    struct install_journal full;
    install_journal_init(&full, notify_cb, &full_log);
    struct install_journal_backend full_backend = backend_of(&full_ram);
    CHECK(!install_journal_bind(&full, &full_backend),
          "a full resumed journal does not bind ready");
    CHECK(full_ram.writes == 0U && full_ram.overruns == 0U,
          "a journal with no room writes nothing past the end");
    CHECK(full.state == INSTALL_JOURNAL_FULL,
          "the full resume is named on the notify side");
    CHECK(full_log.count == 1U &&
              strcmp(full_log.lines[0],
                     "[INSTALL-LOG] state=FULL reason=CAPACITY") == 0,
          "FULL is named once, with the capacity as the reason");
    install_journal_observe(&full, "late", 4U);
    CHECK(full.refused == 1U, "records after the bound are counted");

    /* The panic path obeys the same bound: it replays nothing once the journal
       is full, and the flush still reaches the placement (requirement 2). */
    struct ram_backend panic_ram = {.capacity = 4U};
    struct notify_log panic_log = {0};
    struct install_journal panic_journal;
    install_journal_init(&panic_journal, notify_cb, &panic_log);
    struct install_journal_backend panic_backend = backend_of(&panic_ram);
    for (uint32_t i = 0U; i < 32U; ++i) {
        char line[40];
        int length = snprintf(line, sizeof(line), "[EVENT] tail=%u", i);
        install_journal_observe(&panic_journal, line, (size_t)length);
    }
    (void)install_journal_bind(&panic_journal, &panic_backend);
    CHECK(panic_ram.writes == 4U && panic_ram.overruns == 0U,
          "the replay behind a full bind stops at the capacity");
    CHECK(install_journal_panic_flush(&panic_journal),
          "the panic flush still reaches the placement");
    CHECK(panic_ram.writes == 4U && panic_ram.overruns == 0U,
          "and the panic replay writes nothing past the capacity either");

    /* A refused write is not a full journal: the placement is still there, so
       the staged records stay in the ring and reach the medium when a later
       bind resumes it.  Requirement 1 staged the machine announcement and the
       observed storage precisely so that one failed write does not lose
       them - the record the placement refused goes back to the front. */
    struct ram_backend retry_ram = {.capacity = 16U};
    struct notify_log retry_log = {0};
    struct install_journal retry;
    install_journal_init(&retry, notify_cb, &retry_log);
    struct install_journal_backend retry_backend = backend_of(&retry_ram);
    install_journal_observe(&retry, "[BOOT] machine cpus=1", 20U);
    install_journal_observe(&retry, "[STORAGE-REPORT] found=0", 23U);
    install_journal_observe(&retry, "[PRODUCTION-INSTALL] refused", 29U);
    retry_ram.fail_after = 2U; /* the header, then the first record, refused */
    CHECK(!install_journal_bind(&retry, &retry_backend),
          "a bind whose replay is refused is not reported ready");
    CHECK(retry.state == INSTALL_JOURNAL_UNAVAILABLE && retry_log.count == 1U &&
              strcmp(retry_log.lines[0],
                     "[INSTALL-LOG] state=UNAVAILABLE reason=WRITE") == 0,
          "the refused write is named as a write, not as a capacity");
    CHECK(retry.pending_count == 3U && retry.refused == 0U,
          "no staged record is counted away by one refused write");
    CHECK(retry_ram.writes == 2U && retry_ram.committed == 1U,
          "only the header reached the medium");
    retry_ram.fail_after = 0U;
    CHECK(install_journal_bind(&retry, &retry_backend),
          "a later bind resumes the placement");
    char resumed[INSTALL_JOURNAL_SLOT_BYTES];
    CHECK(retry_ram.committed == 4U && retry.pending_count == 0U,
          "the kept records reach the medium on the resumed bind");
    CHECK(record_text(&retry_ram, 1U, resumed, sizeof(resumed)) != NULL &&
              strncmp(resumed, "[BOOT] machine", 14U) == 0,
          "the oldest staged record goes first");
    CHECK(record_text(&retry_ram, 3U, resumed, sizeof(resumed)) != NULL &&
              strncmp(resumed, "[PRODUCTION-INSTALL]", 20U) == 0,
          "and the ring keeps its order across the failure");
}

static void test_reentrancy_guard(void)
{
    struct ram_backend ram = {.capacity = 32U, .reenter = true};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);
    ram.journal = &journal;
    CHECK(install_journal_bind(&journal, &backend), "bind with a re-entrant backend");
    uint32_t committed = ram.committed;
    install_journal_observe(&journal, "outer", 5U);
    CHECK(ram.committed == committed + 1U,
          "the re-entrant record is ignored, not written");
}

static void test_truncation_is_named(void)
{
    struct ram_backend ram = {.capacity = 32U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);
    CHECK(install_journal_bind(&journal, &backend), "bind before the long record");

    char long_record[INSTALL_JOURNAL_TEXT_MAX + 64U];
    memset(long_record, 'x', sizeof(long_record) - 1U);
    long_record[sizeof(long_record) - 1U] = '\0';
    install_journal_observe(&journal, long_record, sizeof(long_record) - 1U);
    CHECK(journal.truncated == 1U, "the clamp is counted");
    char buffer[INSTALL_JOURNAL_SLOT_BYTES];
    CHECK(record_text(&ram, 1U, buffer, sizeof(buffer)) != NULL &&
              strncmp(buffer, "[INSTALL-LOG] state=TRUNCATED limit=440", 38U) ==
                  0,
          "the clamp is named before the shortened record");
    size_t length = 0U;
    CHECK(decode_record(ram.slots[2], buffer, sizeof(buffer), &length) &&
              length == INSTALL_JOURNAL_TEXT_MAX,
          "the shortened record carries the bound as its length");
}

static void test_resumed_journal_has_no_second_header(void)
{
    struct ram_backend ram = {.capacity = 32U, .committed = 7U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);
    CHECK(install_journal_bind(&journal, &backend), "bind a resumed journal");
    CHECK(journal.written == 7U, "the resumed journal continues from its slot");
    CHECK(ram.committed == 7U, "no header is written into the middle (F6)");
    install_journal_observe(&journal, "after resume", 12U);
    CHECK(ram.committed == 8U, "the next record continues the stream");
}

static void test_header_layout(void)
{
    struct ram_backend ram = {.capacity = 1024U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);
    CHECK(install_journal_bind(&journal, &backend), "bind writes the header");
    const uint8_t *slot = ram.slots[0];
    CHECK(memcmp(slot, INSTALL_JOURNAL_MAGIC, INSTALL_JOURNAL_MAGIC_BYTES) == 0,
          "header magic");
    CHECK(le32_at(&slot[9]) == INSTALL_JOURNAL_VERSION, "header version");
    CHECK(le32_at(&slot[13]) == INSTALL_JOURNAL_SLOT_BYTES, "header slot size");
    CHECK(le32_at(&slot[17]) == 1024U, "header slot count is the capacity");
    CHECK(le32_at(&slot[21]) == 0U, "a fresh journal starts at slot 0");
    CHECK(le32_at(&slot[36]) == install_journal_crc32(slot, 36U),
          "header CRC covers the header fields");
    uint8_t copy[INSTALL_JOURNAL_SLOT_BYTES];
    memcpy(copy, slot, sizeof(copy));
    copy[17] ^= 0x01U;
    CHECK(le32_at(&copy[36]) != install_journal_crc32(copy, 36U),
          "a changed header field breaks the CRC");
}

static void test_flush_and_panic_paths(void)
{
    struct ram_backend ram = {.capacity = 32U};
    struct notify_log log = {0};
    struct install_journal journal;
    install_journal_init(&journal, notify_cb, &log);
    struct install_journal_backend backend = backend_of(&ram);

    CHECK(!install_journal_flush(&journal), "flush before bind is refused");
    CHECK(!install_journal_panic_flush(&journal), "panic before bind is refused");
    CHECK(log.count == 1U &&
              strcmp(log.lines[0],
                     "[INSTALL-LOG] state=UNAVAILABLE reason=PANIC_UNBOUND") == 0,
          "an unwritten panic tail is named, not faked");

    CHECK(install_journal_bind(&journal, &backend), "bind for the panic path");
    install_journal_observe(&journal, "tail", 4U);
    CHECK(install_journal_flush(&journal), "flush on a healthy backend");
    CHECK(ram.flushes == 1U, "flush reached the backend");
    CHECK(install_journal_panic_flush(&journal), "panic flush on a healthy backend");
    CHECK(ram.flushes == 2U, "panic flush flushed again");

    ram.fail_flush = true;
    CHECK(!install_journal_flush(&journal), "a failed flush is reported");
    CHECK(journal.state == INSTALL_JOURNAL_UNAVAILABLE,
          "a failed flush turns the journal off");
    CHECK(log.count == 2U &&
              strcmp(log.lines[1],
                     "[INSTALL-LOG] state=UNAVAILABLE reason=FLUSH") == 0,
          "the failed flush is named");
}

static void test_null_and_state_contract(void)
{
    install_journal_init(NULL, NULL, NULL);
    install_journal_observe(NULL, "x", 1U);
    CHECK(!install_journal_bind(NULL, NULL), "bind with no core is refused");
    CHECK(strcmp(install_journal_state_string(INSTALL_JOURNAL_UNBOUND),
                 "UNBOUND") == 0,
          "UNBOUND token");
    CHECK(strcmp(install_journal_state_string(INSTALL_JOURNAL_READY),
                 "READY") == 0,
          "READY token");
    CHECK(strcmp(install_journal_state_string(INSTALL_JOURNAL_UNAVAILABLE),
                 "UNAVAILABLE") == 0,
          "UNAVAILABLE token");
    CHECK(strcmp(install_journal_state_string(INSTALL_JOURNAL_FULL), "FULL") == 0,
          "FULL token");

    struct install_journal journal;
    install_journal_init(&journal, NULL, NULL);
    install_journal_observe(&journal, NULL, 0U);
    install_journal_observe(&journal, "x", 0U);
    CHECK(journal.pending_count == 0U, "an empty record is not staged");

    /* a torn slot must not decode as text */
    uint8_t slot[INSTALL_JOURNAL_SLOT_BYTES];
    memset(slot, 0, sizeof(slot));
    char buffer[INSTALL_JOURNAL_SLOT_BYTES];
    size_t length = 0U;
    CHECK(!decode_record(slot, buffer, sizeof(buffer), &length),
          "an empty slot is not a record");
}

int main(void)
{
    test_pending_replay_in_order();
    test_ring_bound_and_overflow_named();
    test_write_failure_is_named_and_silent();
    test_placement_refusal_before_write_is_named();
    test_full_is_named_once();
    test_capacity_bounds_every_write_path();
    test_reentrancy_guard();
    test_truncation_is_named();
    test_resumed_journal_has_no_second_header();
    test_header_layout();
    test_flush_and_panic_paths();
    test_null_and_state_contract();
    printf("install-journal-core-host: %u checks, %u failures\n", checks,
           failures);
    return failures == 0U ? 0 : 1;
}
