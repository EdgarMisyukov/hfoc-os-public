#include "appliance_boot_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_BOOT_LOG_HOST_TEST

/*
 * ITEM-410: the boot log's own proof.
 *
 * The appliance writes the log to a preallocated file on its EFI System
 * Partition, and a hardware run reads it back afterwards - out of a mounted
 * ESP with `cat`, or out of the raw image with
 * `scripts/hfoc-esp-read.py --bootlog`.  So the thing that has to be exact is
 * the *format*: the slot layout, its checksum, the wrap the sequence numbers
 * and the generation describe, and the honesty of the reader rules when the
 * file is torn, truncated or lying about its own size.
 *
 * Everything here is arithmetic over byte arrays: an "extent" in this proof is
 * a plain array of slots, written through the same ring and the same write
 * plan the store uses, and read back through the same parser the reader is
 * modelled on.
 */

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* ------------------------------------------------------------- the extent -- */

struct extent {
    unsigned char slots[APPLIANCE_BOOT_LOG_SLOT_COUNT]
                       [APPLIANCE_BOOT_LOG_SLOT_SIZE];
};

/* One extent, zeroed per case: the file the appliance would have written. */
static struct extent extent;

static struct appliance_boot_log_header fresh_header(void)
{
    struct appliance_boot_log_header header;

    memset(&header, 0, sizeof(header));
    header.version = APPLIANCE_BOOT_LOG_VERSION;
    header.slot_size = APPLIANCE_BOOT_LOG_SLOT_SIZE;
    header.slot_count = APPLIANCE_BOOT_LOG_SLOT_COUNT;
    header.record_capacity = APPLIANCE_BOOT_LOG_RECORD_CAPACITY;
    return header;
}

/*
 * One write of one pending record - the exact sequence the store performs:
 * ask the ring for the next unwritten record and the slot it lands in, render
 * the slot, write it into the extent, advance the header's cursor and
 * generation, and only then retire the record in the ring.
 */
static bool extent_write_next(
    struct extent *extent,
    struct appliance_boot_log_ring *ring,
    struct appliance_boot_log_header *header)
{
    struct appliance_boot_log_pending_entry pending;
    struct appliance_boot_log_write_plan plan;
    struct appliance_boot_log_entry entry;
    enum appliance_boot_log_status status;

    status = appliance_boot_log_ring_next(ring, header, &pending, &plan);
    if (status != APPLIANCE_BOOT_LOG_OK) {
        return false;
    }
    memset(&entry, 0, sizeof(entry));
    entry.sequence = pending.sequence;
    entry.generation = plan.generation;
    entry.ticks = pending.ticks;
    entry.length = pending.length;
    memcpy(entry.text, pending.text, pending.length);
    if (appliance_boot_log_render_record(
            &entry, extent->slots[plan.slot_index]) !=
        APPLIANCE_BOOT_LOG_OK) {
        return false;
    }
    if (appliance_boot_log_advance_cursor(
            header->cursor, &header->cursor, &header->generation) !=
        APPLIANCE_BOOT_LOG_OK) {
        return false;
    }
    header->next_sequence = ring->next_sequence;
    header->written = ring->written_sequence + 1U;
    header->dropped = ring->dropped;
    return appliance_boot_log_ring_mark_written(ring) ==
           APPLIANCE_BOOT_LOG_OK;
}

static void extent_flush(
    struct extent *extent,
    struct appliance_boot_log_ring *ring,
    struct appliance_boot_log_header *header)
{
    while (extent_write_next(extent, ring, header)) {
        continue;
    }
    CHECK(appliance_boot_log_render_header(
              header, extent->slots[0]) == APPLIANCE_BOOT_LOG_OK,
          "the header of the flushed extent renders");
}

/*
 * The reader's rule, reduced to what makes it honest: the newest valid record
 * is the end of the log, and the run that precedes it - slot by slot, sequence
 * by sequence, backwards - is the log.  A slot that does not parse ends the
 * run; it is never printed.
 */
struct extent_scan {
    size_t valid;
    size_t torn;
    size_t run_first_slot;
    size_t run_slots;
    uint64_t run_oldest;
    uint64_t run_newest;
    uint64_t run_generation;
    bool contiguous;
};

static struct extent_scan extent_scan(const struct extent *extent)
{
    struct extent_scan scan;
    struct appliance_boot_log_entry entries[APPLIANCE_BOOT_LOG_SLOT_COUNT];
    enum appliance_boot_log_slot_status status[APPLIANCE_BOOT_LOG_SLOT_COUNT];
    size_t index;
    size_t newest = 0U;
    bool have_newest = false;

    memset(&scan, 0, sizeof(scan));
    memset(entries, 0, sizeof(entries));
    memset(status, 0, sizeof(status));
    for (index = 1U; index < APPLIANCE_BOOT_LOG_SLOT_COUNT; ++index) {
        status[index] = appliance_boot_log_parse_slot(
            extent->slots[index], &entries[index]);
        if (status[index] == APPLIANCE_BOOT_LOG_SLOT_OK) {
            ++scan.valid;
        } else if (status[index] == APPLIANCE_BOOT_LOG_SLOT_TORN) {
            ++scan.torn;
        }
    }
    for (index = 1U; index < APPLIANCE_BOOT_LOG_SLOT_COUNT; ++index) {
        if (status[index] != APPLIANCE_BOOT_LOG_SLOT_OK) {
            continue;
        }
        if (!have_newest || entries[index].sequence >=
                                entries[newest].sequence) {
            newest = index;
            have_newest = true;
        }
    }
    if (!have_newest) {
        return scan;
    }
    scan.run_newest = entries[newest].sequence;
    scan.run_generation = entries[newest].generation;
    scan.run_first_slot = newest;
    scan.run_slots = 1U;
    scan.run_oldest = entries[newest].sequence;
    scan.contiguous = true;
    while (true) {
        size_t previous = scan.run_first_slot == 1U
                              ? APPLIANCE_BOOT_LOG_SLOT_COUNT - 1U
                              : scan.run_first_slot - 1U;

        if (status[previous] != APPLIANCE_BOOT_LOG_SLOT_OK ||
            entries[previous].sequence + 1U != scan.run_oldest) {
            scan.contiguous = status[previous] !=
                              APPLIANCE_BOOT_LOG_SLOT_OK;
            break;
        }
        scan.run_first_slot = previous;
        scan.run_oldest = entries[previous].sequence;
        ++scan.run_slots;
    }
    return scan;
}

/* --------------------------------------------------------------- the tests -- */

static void test_crc32(void)
{
    static const char *const known = "123456789";
    static const char *const sentence =
        "the quick brown fox jumps over the lazy dog";
    char text[APPLIANCE_BOOT_LOG_TEXT_MAX + 1U];

    CHECK(appliance_boot_log_crc32((const unsigned char *)known, 9U) ==
              UINT32_C(0xCBF43926),
          "the boot log checksum is the CRC-32 the host reader computes");
    CHECK(appliance_boot_log_crc32(NULL, 4U) == 0U,
          "the boot log checksum refuses a null buffer");
    CHECK(appliance_boot_log_crc32((const unsigned char *)sentence,
                                   strlen(sentence)) ==
              UINT32_C(0xCE0C5114),
          "the boot log checksum matches the known vector of the sentence");
    memset(text, 'a', sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';
    CHECK(appliance_boot_log_crc32((const unsigned char *)text,
                                   strlen(text)) ==
              appliance_boot_log_crc32_string(text),
          "the string form of the checksum is the buffer form");
}

static void test_header_round_trip(void)
{
    struct appliance_boot_log_header header = fresh_header();
    struct appliance_boot_log_header parsed;
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE];

    header.generation = 3U;
    header.next_sequence = 42U;
    header.cursor = 7U;
    header.dropped = 1U;
    header.written = 41U;
    CHECK(appliance_boot_log_render_header(&header, slot) ==
              APPLIANCE_BOOT_LOG_OK,
          "the boot log header renders");
    CHECK(memcmp(slot, "HFOC-BOOTLOG v=1 slot=512 slots=512 records=511 gen=",
                 47U) == 0,
          "the header names the format, the geometry and the generation");
    CHECK(slot[APPLIANCE_BOOT_LOG_SLOT_SIZE - 1U] == 0U,
          "the header line is padded with NUL bytes inside its slot");
    CHECK(appliance_boot_log_parse_header(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_OK,
          "the boot log header parses back");
    CHECK(parsed.version == APPLIANCE_BOOT_LOG_VERSION &&
              parsed.slot_size == APPLIANCE_BOOT_LOG_SLOT_SIZE &&
              parsed.slot_count == APPLIANCE_BOOT_LOG_SLOT_COUNT &&
              parsed.record_capacity ==
                  APPLIANCE_BOOT_LOG_RECORD_CAPACITY &&
              parsed.generation == 3U && parsed.next_sequence == 42U &&
              parsed.cursor == 7U && parsed.dropped == 1U &&
              parsed.written == 41U,
          "every header field survives the round trip");

    slot[10U] ^= 0x01U;
    CHECK(appliance_boot_log_parse_header(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_BAD_SLOT,
          "a header whose bytes were changed does not parse");
    CHECK(appliance_boot_log_render_header(&header, slot) ==
              APPLIANCE_BOOT_LOG_OK,
          "the header renders again after the corruption");
    {
        unsigned char *crc_digits = (unsigned char *)memchr(
            slot, 'c', APPLIANCE_BOOT_LOG_SLOT_SIZE);
        size_t offset = (size_t)(crc_digits - slot) + 4U;

        slot[offset] = (unsigned char)(slot[offset] == (unsigned char)'0'
                                           ? '1'
                                           : '0');
    }
    CHECK(appliance_boot_log_parse_header(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_BAD_SLOT,
          "a header with a wrong checksum does not parse");
    memset(slot, 0, sizeof(slot));
    CHECK(appliance_boot_log_parse_header(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_EMPTY,
          "an all-zero header slot is empty, not corrupt");
    CHECK(appliance_boot_log_render_header(NULL, slot) ==
              APPLIANCE_BOOT_LOG_NULL,
          "the header render refuses a null header");
    CHECK(appliance_boot_log_render_header(&header, NULL) ==
              APPLIANCE_BOOT_LOG_NULL,
          "the header render refuses a null slot");
    header.cursor = APPLIANCE_BOOT_LOG_RECORD_CAPACITY;
    CHECK(appliance_boot_log_render_header(&header, slot) ==
              APPLIANCE_BOOT_LOG_BAD_SLOT,
          "a header with a cursor outside the record slots is refused");
}

static void test_record_round_trip(void)
{
    struct appliance_boot_log_entry entry;
    struct appliance_boot_log_entry parsed;
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE];
    char text[APPLIANCE_BOOT_LOG_TEXT_MAX + 1U];

    memset(&entry, 0, sizeof(entry));
    entry.sequence = 5U;
    entry.generation = 2U;
    entry.ticks = 123456U;
    entry.length = strlen("[PRODUCTION-DHCP] state=BOUND lease=ACQUIRED");
    memcpy(entry.text, "[PRODUCTION-DHCP] state=BOUND lease=ACQUIRED",
           entry.length);
    CHECK(appliance_boot_log_render_record(&entry, slot) ==
              APPLIANCE_BOOT_LOG_OK,
          "a boot log record renders");
    CHECK(strncmp((const char *)slot,
                  "@00000000000000000005 g0000000002 "
                  "t00000000000000123456 l047 c",
                  57U) == 0,
          "the record line carries its sequence, generation, ticks and length");
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_OK,
          "a rendered record parses back");
    CHECK(parsed.sequence == entry.sequence &&
              parsed.generation == entry.generation &&
              parsed.ticks == entry.ticks &&
              parsed.length == entry.length &&
              strcmp(parsed.text, entry.text) == 0,
          "every record field survives the round trip");

    memset(text, 'x', APPLIANCE_BOOT_LOG_TEXT_MAX);
    text[APPLIANCE_BOOT_LOG_TEXT_MAX] = '\0';
    memset(&entry, 0, sizeof(entry));
    entry.sequence = UINT64_C(18446744073709551615);
    entry.ticks = UINT64_C(18446744073709551615);
    entry.length = APPLIANCE_BOOT_LOG_TEXT_MAX;
    memcpy(entry.text, text, entry.length);
    CHECK(appliance_boot_log_render_record(&entry, slot) ==
              APPLIANCE_BOOT_LOG_OK,
          "a record at the text bound and at the sequence bound renders");
    CHECK(slot[APPLIANCE_BOOT_LOG_SLOT_SIZE - 1U] == (unsigned char)'\n',
          "a full record ends exactly at the last byte of its slot");
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_OK &&
              parsed.length == APPLIANCE_BOOT_LOG_TEXT_MAX &&
              parsed.sequence == UINT64_C(18446744073709551615),
          "the maximal record parses back unchanged");

    entry.length = APPLIANCE_BOOT_LOG_TEXT_MAX + 1U;
    memcpy(entry.text, text, APPLIANCE_BOOT_LOG_TEXT_MAX);
    entry.text[APPLIANCE_BOOT_LOG_TEXT_MAX] = 'y';
    CHECK(appliance_boot_log_render_record(&entry, slot) ==
              APPLIANCE_BOOT_LOG_BAD_TEXT,
          "text past the slot bound is refused instead of truncated");
    entry.length = 4U;
    memcpy(entry.text, "ok\ty", 4U);
    CHECK(appliance_boot_log_render_record(&entry, slot) ==
              APPLIANCE_BOOT_LOG_BAD_TEXT,
          "a control character is refused");
    entry.length = 4U;
    memcpy(entry.text, "ok\xffy", 4U);
    CHECK(appliance_boot_log_render_record(&entry, slot) ==
              APPLIANCE_BOOT_LOG_BAD_TEXT,
          "a byte the reader could not render is refused");
    entry.length = 0U;
    CHECK(appliance_boot_log_render_record(&entry, slot) ==
              APPLIANCE_BOOT_LOG_OK,
          "an empty record is a legal record");
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_OK && parsed.length == 0U,
          "an empty record parses back as empty text");
}

static void test_torn_slots_are_never_text(void)
{
    struct appliance_boot_log_entry entry;
    struct appliance_boot_log_entry parsed;
    unsigned char slot[APPLIANCE_BOOT_LOG_SLOT_SIZE];
    unsigned char original[APPLIANCE_BOOT_LOG_SLOT_SIZE];

    memset(&entry, 0, sizeof(entry));
    entry.sequence = 9U;
    entry.generation = 0U;
    entry.ticks = 77U;
    entry.length = strlen("[APPLIANCE-BOOT] runtime=READY storage=READY");
    memcpy(entry.text, "[APPLIANCE-BOOT] runtime=READY storage=READY",
           entry.length);
    CHECK(appliance_boot_log_render_record(&entry, slot) ==
              APPLIANCE_BOOT_LOG_OK,
          "the torn-slot fixture renders");
    memcpy(original, slot, sizeof(original));

    /* A power cut in the middle of the text: the CRC must catch it. */
    slot[80U] ^= 0x01U;
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_TORN,
          "a record whose text was changed is torn and is not text");
    memcpy(slot, original, sizeof(slot));
    slot[APPLIANCE_BOOT_LOG_SLOT_SIZE - 1U] = (unsigned char)'z';
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_TORN,
          "a record with a byte past its newline is torn");
    memcpy(slot, original, sizeof(slot));
    slot[70U] = (unsigned char)'#';
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_TORN,
          "a record whose text does not start after the checksum is torn");
    memcpy(slot, original, sizeof(slot));
    slot[3U] = 0U;
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_TORN,
          "a record whose field header was destroyed is torn");
    memcpy(slot, original, sizeof(slot));
    slot[0U] = 0U;
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_TORN,
          "a slot that only looks empty is torn, not empty");
    memset(slot, 0, sizeof(slot));
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_EMPTY,
          "a slot that was never written is empty");
    /* A truncated tail: the slot cut off inside its own text. */
    memcpy(slot, original, sizeof(slot));
    memset(slot + 90U, 0, APPLIANCE_BOOT_LOG_SLOT_SIZE - 90U);
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_TORN,
          "a slot truncated in the middle is torn");
    CHECK(appliance_boot_log_parse_slot(NULL, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_TORN,
          "a missing slot is torn rather than text");
    memset(slot, 0, sizeof(slot));
    memcpy(slot, "@00000000000000000000000000000000000000 g", 43U);
    CHECK(appliance_boot_log_parse_slot(slot, &parsed) ==
              APPLIANCE_BOOT_LOG_SLOT_TORN,
          "a record line that stops inside its fields is torn");
}

static void test_cursor_arithmetic(void)
{
    uint64_t cursor = 0U;
    uint64_t generation = 0U;

    CHECK(appliance_boot_log_slot_for_cursor(0U) == 1U,
          "the first record lands in the first slot after the header");
    CHECK(appliance_boot_log_slot_for_cursor(
              APPLIANCE_BOOT_LOG_RECORD_CAPACITY - 1U) ==
              APPLIANCE_BOOT_LOG_SLOT_COUNT - 1U,
          "the last record lands in the last slot of the extent");
    CHECK(appliance_boot_log_slot_for_cursor(
              APPLIANCE_BOOT_LOG_RECORD_CAPACITY) == 1U,
          "a wrapped cursor starts the extent again");
    CHECK(appliance_boot_log_advance_cursor(5U, &cursor, &generation) ==
              APPLIANCE_BOOT_LOG_OK && cursor == 6U && generation == 0U,
          "advancing inside the extent does not change the generation");
    CHECK(appliance_boot_log_advance_cursor(
              APPLIANCE_BOOT_LOG_RECORD_CAPACITY - 1U, &cursor,
              &generation) == APPLIANCE_BOOT_LOG_OK &&
              cursor == 0U && generation == 1U,
          "the wrap starts the extent again and marks the generation");
    CHECK(appliance_boot_log_advance_cursor(
              APPLIANCE_BOOT_LOG_RECORD_CAPACITY, &cursor,
              &generation) == APPLIANCE_BOOT_LOG_BAD_RANGE,
          "a cursor outside the extent is refused");
    CHECK(appliance_boot_log_advance_cursor(0U, NULL, &generation) ==
              APPLIANCE_BOOT_LOG_NULL,
          "the cursor arithmetic refuses a null output");
}

static void test_ring_staging_and_overflow(void)
{
    struct appliance_boot_log_ring ring;
    struct appliance_boot_log_pending_entry entry;
    struct appliance_boot_log_write_plan plan;
    struct appliance_boot_log_header header = fresh_header();
    uint64_t sequence = 0U;
    uint32_t pending = 0U;
    uint64_t dropped = 0U;
    size_t index;
    char text[64U];

    memset(&ring, 0, sizeof(ring));
    CHECK(appliance_boot_log_ring_append(&ring, "x", 1U, 0U, NULL) ==
              APPLIANCE_BOOT_LOG_BAD_STATE,
          "the staging ring refuses an append before it is initialised");
    CHECK(appliance_boot_log_ring_init(&ring) == APPLIANCE_BOOT_LOG_OK,
          "the staging ring initialises");
    for (index = 0U; index < 5U; ++index) {
        const int written = snprintf(text, sizeof(text),
                                     "[BOOT] record %u", (unsigned)index);

        CHECK(appliance_boot_log_ring_append(
                  &ring, text, (size_t)written, 100U + index,
                  &sequence) == APPLIANCE_BOOT_LOG_OK,
              "a record is staged with the sequence the flush will write");
        CHECK(sequence == (uint64_t)index,
              "the staged sequence is the next unwritten one");
    }
    CHECK(appliance_boot_log_ring_state(&ring, &pending, &dropped) ==
              APPLIANCE_BOOT_LOG_OK && pending == 5U && dropped == 0U,
          "five staged records are five pending records");
    for (index = 0U; index < 5U; ++index) {
        CHECK(appliance_boot_log_ring_next(&ring, &header, &entry, &plan) ==
                  APPLIANCE_BOOT_LOG_OK,
              "the flush walks the pending records oldest first");
        const uint64_t cursor = header.cursor;

        CHECK(entry.sequence == (uint64_t)index &&
                  plan.sequence == (uint64_t)index &&
                  plan.slot_index ==
                      appliance_boot_log_slot_for_cursor(cursor) &&
                  plan.generation == header.generation,
              "each pending record names its own slot and generation");
        CHECK(appliance_boot_log_ring_mark_written(&ring) ==
                  APPLIANCE_BOOT_LOG_OK,
              "a written record is retired from the ring");
        CHECK(appliance_boot_log_advance_cursor(
                  cursor, &header.cursor, &header.generation) ==
                  APPLIANCE_BOOT_LOG_OK,
              "the extent cursor advances with the record");
    }
    CHECK(appliance_boot_log_ring_next(&ring, &header, &entry, &plan) ==
              APPLIANCE_BOOT_LOG_EMPTY,
          "a flushed ring has nothing left to write");
    CHECK(appliance_boot_log_ring_state(&ring, &pending, &dropped) ==
              APPLIANCE_BOOT_LOG_OK && pending == 0U && dropped == 0U,
          "the ring owes the extent nothing after a flush");

    /*
     * An appliance whose storage never came up: the ring fills and drops the
     * oldest records.  The loss is counted, and a later flush skips exactly
     * the sequences that are gone instead of writing somebody else's text.
     */
    for (index = 0U; index < APPLIANCE_BOOT_LOG_PENDING_CAPACITY + 6U;
         ++index) {
        const int written = snprintf(text, sizeof(text), "[NET] poll %u",
                                     (unsigned)index);

        CHECK(appliance_boot_log_ring_append(
                  &ring, text, (size_t)written, 1U, &sequence) ==
                  APPLIANCE_BOOT_LOG_OK,
              "a record past the ring capacity is still accepted");
    }
    CHECK(sequence == 5U + APPLIANCE_BOOT_LOG_PENDING_CAPACITY + 5U,
          "the sequence keeps counting over the dropped records");
    CHECK(ring.dropped == 6U,
          "the ring counts the records it had to drop");
    CHECK(appliance_boot_log_ring_state(&ring, &pending, &dropped) ==
              APPLIANCE_BOOT_LOG_OK &&
              pending == APPLIANCE_BOOT_LOG_PENDING_CAPACITY &&
              dropped == 6U,
          "the ring holds exactly its capacity after the overflow");
    CHECK(appliance_boot_log_ring_next(&ring, &header, &entry, &plan) ==
              APPLIANCE_BOOT_LOG_OK &&
              entry.sequence == 11U &&
              strcmp(entry.text, "[NET] poll 6") == 0,
          "the flush starts at the oldest record the ring still holds");
    CHECK(appliance_boot_log_ring_entry(&ring, 0U, &entry) ==
              APPLIANCE_BOOT_LOG_EMPTY,
          "a dropped record is not resident in the ring");
    CHECK(appliance_boot_log_ring_entry(&ring, 11U, &entry) ==
              APPLIANCE_BOOT_LOG_OK,
          "a resident record can be read back by sequence");
    CHECK(appliance_boot_log_ring_append(&ring, "bad\ntext", 8U, 0U, NULL) ==
              APPLIANCE_BOOT_LOG_BAD_TEXT,
          "a staged record carrying a newline is refused");
    CHECK(appliance_boot_log_ring_append(&ring, NULL, 4U, 0U, NULL) ==
              APPLIANCE_BOOT_LOG_NULL,
          "the staging ring refuses a null record");
    CHECK(appliance_boot_log_ring_append(&ring, "x", 0U, 0U, NULL) ==
              APPLIANCE_BOOT_LOG_BAD_TEXT,
          "the staging ring refuses an empty record");
}

static void test_small_ring_overflow_skips_the_gap(void)
{
    /*
     * The same rule at the ring's own size: 64 staged records, 64 written, and
     * the 65th staged pushes the first out.  A flush then starts at sequence 1
     * and never writes a sequence the ring no longer holds.
     */
    struct appliance_boot_log_ring ring;
    struct appliance_boot_log_pending_entry entry;
    struct appliance_boot_log_write_plan plan;
    struct appliance_boot_log_header header = fresh_header();
    uint64_t dropped = 0U;
    size_t index;

    CHECK(appliance_boot_log_ring_init(&ring) == APPLIANCE_BOOT_LOG_OK,
          "the overflow ring initialises");
    for (index = 0U; index < APPLIANCE_BOOT_LOG_PENDING_CAPACITY + 1U;
         ++index) {
        CHECK(appliance_boot_log_ring_append(
                  &ring, "x", 1U, 0U, NULL) == APPLIANCE_BOOT_LOG_OK,
              "the overflow ring stages a record");
    }
    CHECK(appliance_boot_log_ring_state(&ring, NULL, &dropped) ==
              APPLIANCE_BOOT_LOG_OK && dropped == 1U,
          "the flush plan reports the one record the ring had to drop");
    CHECK(appliance_boot_log_ring_next(&ring, &header, &entry, &plan) ==
              APPLIANCE_BOOT_LOG_OK && entry.sequence == 1U,
          "the flush resumes at the oldest surviving record");
}

static void test_flush_policy(void)
{
    CHECK(appliance_boot_log_flush_trigger(
              "[BOOT-CHAIN] verdict=ATTESTED reason=NONE",
              strlen("[BOOT-CHAIN] verdict=ATTESTED reason=NONE")),
          "the boot chain record is flushed at once");
    CHECK(appliance_boot_log_flush_trigger(
              "[APPLIANCE-BOOT] runtime=READY identity=OK",
              strlen("[APPLIANCE-BOOT] runtime=READY identity=OK")),
          "the appliance readiness record is flushed at once");
    CHECK(appliance_boot_log_flush_trigger(
              "[PRODUCTION-DHCP] state=BOUND lease=ACQUIRED address=10.0.2.15",
              strlen("[PRODUCTION-DHCP] state=BOUND lease=ACQUIRED "
                     "address=10.0.2.15")),
          "the DHCP record is flushed at once");
    CHECK(appliance_boot_log_flush_trigger(
              "[PRODUCTION-DNS] state=RESOLVED answer=10.0.2.3",
              strlen("[PRODUCTION-DNS] state=RESOLVED answer=10.0.2.3")),
          "the DNS record is flushed at once");
    CHECK(appliance_boot_log_flush_trigger(
              "[PRODUCTION-SERVING] v=1 state=SERVING",
              strlen("[PRODUCTION-SERVING] v=1 state=SERVING")),
          "the serving publication is flushed at once");
    CHECK(appliance_boot_log_flush_trigger(
              "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE",
              strlen("[PRODUCTION-INSTALL] boot-entry state=AVAILABLE")),
          "the install record is flushed at once");
    CHECK(appliance_boot_log_flush_trigger(
              "[STORAGE] state=UNAVAILABLE reason=NO_DEVICE",
              strlen("[STORAGE] state=UNAVAILABLE reason=NO_DEVICE")),
          "a record that announces an unavailable owner is flushed at once");
    CHECK(appliance_boot_log_flush_trigger(
              "[SERVICE] state=REFUSED reason=NO_KEY",
              strlen("[SERVICE] state=REFUSED reason=NO_KEY")),
          "a refusal is flushed at once");
    CHECK(appliance_boot_log_flush_trigger(
              "[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK",
              strlen("[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK")),
          "the storage verdict is flushed at once");
    /*
     * ITEM-452: every device the xHCI walk reaches names itself on its own
     * `[USB ]` line, and a machine whose pointer is plugged in but not bound
     * has to be readable from the disk it came up on.  A bound device's line
     * carries no "state=UNAVAILABLE/REFUSED" fragment, so without the marker
     * itself it would wait for the next flush - which on a machine that is
     * switched off right after a failed run is never.
     */
    CHECK(appliance_boot_log_flush_trigger(
              "[USB ] pci=05:00.3 port=2 speed=FULL role=MOUSE state=BOUND "
              "reason=OK vid=0x000000000000046D ifaces=00:03/01/02",
              strlen("[USB ] pci=05:00.3 port=2 speed=FULL role=MOUSE "
                     "state=BOUND reason=OK vid=0x000000000000046D "
                     "ifaces=00:03/01/02")),
          "a device record reaches the durable log at once");
    CHECK(!appliance_boot_log_flush_trigger(
              "[AHCI] state=READY ports=4 devices=1",
              strlen("[AHCI] state=READY ports=4 devices=1")),
          "an ordinary healthy record waits for the next flush");
    CHECK(!appliance_boot_log_flush_trigger(
              "[KBD ] state=READY reason=OK source=PS2",
              strlen("[KBD ] state=READY reason=OK source=PS2")),
          "a reason field alone does not make a record urgent");
    CHECK(!appliance_boot_log_flush_trigger(
              "[BOOTLOG] state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN "
              "next=12 written=12",
              strlen("[BOOTLOG] state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN "
                     "next=12 written=12")),
          "the log's own state record does not trigger a flush of its own");
    CHECK(!appliance_boot_log_flush_trigger(NULL, 0U),
          "the flush policy refuses a null record");
    CHECK(!appliance_boot_log_flush_trigger("", 0U),
          "the flush policy refuses an empty record");
}

static void test_written_extent_and_wrap(void)
{
    struct appliance_boot_log_ring ring;
    struct appliance_boot_log_header header = fresh_header();
    struct extent_scan scan;
    size_t index;
    char text[64U];

    memset(&extent, 0, sizeof(extent));
    CHECK(appliance_boot_log_ring_init(&ring) == APPLIANCE_BOOT_LOG_OK,
          "the wrap fixture ring initialises");
    for (index = 0U;
         index < APPLIANCE_BOOT_LOG_RECORD_CAPACITY + 5U; ++index) {
        const int written = snprintf(text, sizeof(text), "[BOOT] record %u",
                                     (unsigned)index);

        CHECK(appliance_boot_log_ring_append(
                  &ring, text, (size_t)written, 1000U + index, NULL) ==
                  APPLIANCE_BOOT_LOG_OK,
              "the wrap fixture stages a record");
        /* The staging ring is smaller than the extent: flush as it fills, the
         * way the appliance flushes at each moment that matters. */
        if (index % 32U == 31U) {
            extent_flush(&extent, &ring, &header);
        }
    }
    extent_flush(&extent, &ring, &header);
    CHECK(header.generation == 1U && header.cursor == 5U,
          "writing past the last slot wraps the cursor and the generation");
    CHECK(header.written ==
              APPLIANCE_BOOT_LOG_RECORD_CAPACITY + 5U,
          "the header counts every record the extent ever received");
    scan = extent_scan(&extent);
    CHECK(scan.valid == APPLIANCE_BOOT_LOG_RECORD_CAPACITY &&
              scan.torn == 0U,
          "a wrapped extent still holds one full generation of records");
    CHECK(scan.run_slots == APPLIANCE_BOOT_LOG_RECORD_CAPACITY &&
              scan.run_oldest == 5U &&
              scan.run_newest ==
                  APPLIANCE_BOOT_LOG_RECORD_CAPACITY + 4U,
          "the reader reconstructs the contiguous run the wrap left behind");
    CHECK(scan.run_generation == 1U &&
              extent.slots[1U][0U] == (unsigned char)'@',
          "the newest generation starts at the first record slot");
    {
        struct appliance_boot_log_entry entry;

        CHECK(appliance_boot_log_parse_slot(extent.slots[1U], &entry) ==
                  APPLIANCE_BOOT_LOG_SLOT_OK &&
                  entry.sequence == APPLIANCE_BOOT_LOG_RECORD_CAPACITY &&
                  entry.generation == 1U &&
                  strcmp(entry.text, "[BOOT] record 511") == 0,
              "the slot after the header carries the newest generation");
    }

    /*
     * A torn tail on top of a wrapped extent: the newest slot is a partial
     * write.  The reader must step over it and still find the run that ends at
     * the last record that did make it.
     */
    memset(extent.slots[5U] + 100U, 0xAB,
           APPLIANCE_BOOT_LOG_SLOT_SIZE - 100U);
    scan = extent_scan(&extent);
    CHECK(scan.torn == 1U &&
              scan.run_newest ==
                  APPLIANCE_BOOT_LOG_RECORD_CAPACITY + 3U &&
              scan.run_oldest == 5U && scan.run_slots == 510U,
          "a torn newest slot ends the run: the last complete record is the "
          "newest one the reader reports");
}

static void test_ring_for_a_second_boot(void)
{
    /*
     * Sequences continue across boots: the second boot's ring resumes from the
     * header the first boot left (next/generation/cursor), which is what makes
     * an install record and the boot that followed it readable in one log.
     */
    struct appliance_boot_log_ring first;
    struct appliance_boot_log_ring second;
    struct appliance_boot_log_pending_entry entry;
    struct appliance_boot_log_write_plan plan;
    struct appliance_boot_log_header header = fresh_header();
    size_t index;

    CHECK(appliance_boot_log_ring_init(&first) == APPLIANCE_BOOT_LOG_OK,
          "the first boot's ring initialises");
    for (index = 0U; index < 3U; ++index) {
        CHECK(appliance_boot_log_ring_append(
                  &first, "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE",
                  strlen("[PRODUCTION-INSTALL] boot-entry state=AVAILABLE"),
                  0U, NULL) == APPLIANCE_BOOT_LOG_OK,
              "the install stages a record");
    }
    CHECK(appliance_boot_log_ring_next(&first, &header, &entry, &plan) ==
              APPLIANCE_BOOT_LOG_OK && entry.sequence == 0U &&
              plan.slot_index == 1U,
          "the install's first record lands in the first slot");
    CHECK(appliance_boot_log_ring_mark_written(&first) ==
              APPLIANCE_BOOT_LOG_OK &&
              appliance_boot_log_advance_cursor(header.cursor,
                                                &header.cursor,
                                                &header.generation) ==
                  APPLIANCE_BOOT_LOG_OK,
          "the install's first record is retired");
    header.next_sequence = first.next_sequence;
    header.written = first.written_sequence;
    header.dropped = first.dropped;

    /* The appliance reboots and finds the header the install wrote. */
    CHECK(appliance_boot_log_ring_init(&second) == APPLIANCE_BOOT_LOG_OK,
          "the second boot's ring initialises");
    second.next_sequence = header.next_sequence;
    second.written_sequence = header.next_sequence;
    CHECK(appliance_boot_log_ring_append(
              &second, "[APPLIANCE-BOOT] runtime=READY storage=READY",
              strlen("[APPLIANCE-BOOT] runtime=READY storage=READY"), 10U,
              NULL) == APPLIANCE_BOOT_LOG_OK,
          "the second boot stages the readiness record");
    CHECK(appliance_boot_log_ring_next(&second, &header, &entry, &plan) ==
              APPLIANCE_BOOT_LOG_OK && entry.sequence == 3U &&
              plan.slot_index == 2U,
          "the second boot's record continues the sequence and the extent");
}

/*
 * ------------------------------------------------------------ ITEM-497 -----
 *
 * The input verdict: one readable line per boot, kept for three boots.  What
 * this proof fixes is the text a reader greps and the rule that a carried copy
 * of an earlier boot is the earlier boot's own body, not a summary of one.
 */

static size_t verdict_record(
    unsigned char *slot,
    const char *marker,
    const char *body)
{
    struct appliance_boot_log_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.length = strlen(marker) + strlen(body);
    memcpy(entry.text, marker, strlen(marker));
    memcpy(entry.text + strlen(marker), body, strlen(body));
    entry.text[entry.length] = '\0';
    return appliance_boot_log_render_record(&entry, slot) ==
                   APPLIANCE_BOOT_LOG_OK
               ? entry.length
               : 0U;
}

static void test_input_verdict_text(void)
{
    struct appliance_boot_log_input_state state;
    char body[APPLIANCE_BOOT_LOG_INPUT_BODY_MAX + 1U];
    size_t length;

    CHECK(strcmp(appliance_boot_log_input_verdict("ARMED", "ARMED"),
                 "ARMED") == 0,
          "ITEM-497: both roles armed is ARMED");
    CHECK(strcmp(appliance_boot_log_input_verdict("ARMED", "NO_REPORTS"),
                 "NO_REPORTS") == 0,
          "ITEM-497: an armed but silent role is NO_REPORTS");
    CHECK(strcmp(appliance_boot_log_input_verdict("NOT_ARMED", "ARMED"),
                 "NOT_ARMED") == 0,
          "ITEM-497: never armed outranks silent");
    CHECK(strcmp(appliance_boot_log_input_verdict("ABSENT", "ABSENT"),
                 "NO_DEVICE") == 0,
          "ITEM-497: no HID role at all is NO_DEVICE");
    CHECK(strcmp(appliance_boot_log_input_verdict("ABSENT", "ARMED"),
                 "ARMED") == 0,
          "ITEM-497: an absent role does not fail a machine with a live one");
    CHECK(strcmp(appliance_boot_log_input_verdict("ARMED", "BOGUS"),
                 "NOT_ARMED") == 0,
          "ITEM-497: an unknown role word is not reported as armed");

    memset(&state, 0, sizeof(state));
    state.boot = 17U;
    state.keyboard = "ARMED";
    state.mouse = "NO_REPORTS";
    state.arm_ms = 812U;
    state.first_report_ms = 1340U;
    state.ports = 2U;
    state.reports = 1823U;
    length = appliance_boot_log_format_input_body(body, sizeof(body), &state);
    CHECK(length != 0U, "ITEM-497: the verdict body formats");
    CHECK(strcmp(body,
                 "boot=17 verdict=NO_REPORTS keyboard=ARMED mouse=NO_REPORTS"
                 " arm_ms=812 first_report_ms=1340 ports=2 reports=1823") == 0,
          "ITEM-497: the body names the boot, both roles and both milestones");
    /* The caller refuses rather than truncates: a short buffer is 0. */
    CHECK(appliance_boot_log_format_input_body(body, 16U, &state) == 0U,
          "ITEM-497: a body that does not fit is refused, not truncated");
}

static void test_input_verdict_scan_keeps_every_boot(void)
{
    struct appliance_boot_log_input_record records[2];
    const char *bodies[4] = {
        "boot=1 verdict=NO_DEVICE keyboard=ABSENT mouse=ABSENT arm_ms=0"
        " first_report_ms=0 ports=0 reports=0",
        "boot=2 verdict=NOT_ARMED keyboard=NOT_ARMED mouse=ARMED arm_ms=0"
        " first_report_ms=0 ports=1 reports=12",
        "boot=3 verdict=ARMED keyboard=ARMED mouse=ARMED arm_ms=900"
        " first_report_ms=1200 ports=2 reports=410",
        "boot=4 verdict=ARMED keyboard=ARMED mouse=ARMED arm_ms=700"
        " first_report_ms=800 ports=2 reports=99",
    };
    uint64_t newest_boot = 0U;
    size_t found;

    memset(&extent, 0, sizeof(extent));
    /* Boot 1's own verdict, and the copy boot 2 carried of it. */
    CHECK(verdict_record(extent.slots[1], "[INPUT-PATH] ", bodies[0]) != 0U,
          "ITEM-497: boot 1's verdict renders");
    CHECK(verdict_record(extent.slots[2], "[INPUT-PATH-CARRY] ", bodies[0]) !=
              0U,
          "ITEM-497: boot 2's carried copy renders");
    CHECK(verdict_record(extent.slots[3], "[INPUT-PATH] ", bodies[1]) != 0U,
          "ITEM-497: boot 2's verdict renders");
    CHECK(verdict_record(extent.slots[4], "[INPUT-PATH-CARRY] ", bodies[1]) !=
              0U,
          "ITEM-497: boot 3's carried copy renders");
    CHECK(verdict_record(extent.slots[5], "[INPUT-PATH] ", bodies[2]) != 0U,
          "ITEM-497: boot 3's verdict renders");
    CHECK(verdict_record(extent.slots[6], "[INPUT-PATH] ", bodies[3]) != 0U,
          "ITEM-497: boot 4's verdict renders");
    /* A torn slot carrying a verdict must not be read as one. */
    CHECK(verdict_record(extent.slots[7], "[INPUT-PATH] ",
                         "boot=5 verdict=ARMED keyboard=ARMED mouse=ARMED"
                         " arm_ms=1 first_report_ms=2 ports=1 reports=3") != 0U,
          "ITEM-497: the torn candidate renders first");
    extent.slots[7][40] ^= 0x01U;

    found = appliance_boot_log_scan_input_verdicts(
        &extent.slots[0][0], APPLIANCE_BOOT_LOG_SLOT_COUNT, records, 2U,
        &newest_boot);
    CHECK(found == 2U, "ITEM-497: the scan keeps the two newest boots");
    CHECK(records[0].boot == 4U && records[1].boot == 3U,
          "ITEM-497: the two newest boots are returned newest first");
    CHECK(strcmp(records[0].text, bodies[3]) == 0 &&
              strcmp(records[1].text, bodies[2]) == 0,
          "ITEM-497: a carried copy is the earlier boot's own body");
    CHECK(newest_boot == 4U,
          "ITEM-497: the next boot numbers itself after the newest verdict");

    /* The carried copy of boot 1 is deduplicated against boot 1's own record:
     * with the newest two boots out of the window, boot 1 is one entry. */
    found = appliance_boot_log_scan_input_verdicts(
        &extent.slots[0][0], 4U, records, 2U, &newest_boot);
    CHECK(found == 2U && records[0].boot == 2U && records[1].boot == 1U,
          "ITEM-497: one verdict per boot, whatever copies exist");
    CHECK(strcmp(records[1].text, bodies[0]) == 0,
          "ITEM-497: the deduplicated body is the boot's own verdict");
}

static void test_input_verdict_gate(void)
{
    struct appliance_boot_log_gate gate;

    appliance_boot_log_gate_init(&gate);
    CHECK(appliance_boot_log_gate_admit(&gate, 0U, 1000U, 0U),
          "ITEM-497: the first record of a family is admitted");
    CHECK(!appliance_boot_log_gate_admit(&gate, 0U, 1000U, 999U),
          "ITEM-497: a record inside the interval is held");
    CHECK(appliance_boot_log_gate_admit(&gate, 1U, 1000U, 999U),
          "ITEM-497: the families are independent");
    CHECK(appliance_boot_log_gate_admit(&gate, 0U, 1000U, 1000U),
          "ITEM-497: the interval admits the family again");
    CHECK(appliance_boot_log_gate_suppressed(&gate) == 1U,
          "ITEM-497: what the gate held back is counted");
}

int main(void)
{
    test_crc32();
    test_header_round_trip();
    test_record_round_trip();
    test_torn_slots_are_never_text();
    test_cursor_arithmetic();
    test_ring_staging_and_overflow();
    test_small_ring_overflow_skips_the_gap();
    test_flush_policy();
    test_written_extent_and_wrap();
    test_ring_for_a_second_boot();
    test_input_verdict_text();
    test_input_verdict_scan_keeps_every_boot();
    test_input_verdict_gate();

    if (failures != 0) {
        fprintf(stderr, "%d appliance boot log host test failures\n",
                failures);
        return 1;
    }
    printf("Appliance boot log host tests: ALL OK\n");
    return 0;
}

#endif
