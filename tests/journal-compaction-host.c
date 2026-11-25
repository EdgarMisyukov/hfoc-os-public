#include "journal_compaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_JOURNAL_COMPACTION_HOST_TEST

#define TEST_CAPACITY 4U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static bool bytes_equal(const unsigned char *left,
                        const unsigned char *right,
                        size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool entry_equal(const struct config_kv_entry *entry,
                        uint8_t kind,
                        const char *key,
                        const char *value,
                        uint64_t revision)
{
    size_t key_length = strlen(key);
    size_t value_length = strlen(value);

    return entry != NULL &&
           entry->occupied &&
           entry->kind == kind &&
           entry->key_length == key_length &&
           bytes_equal(entry->key, (const unsigned char *)key, key_length) &&
           entry->value_length == value_length &&
           bytes_equal(entry->value, (const unsigned char *)value,
                       value_length) &&
           entry->revision == revision;
}

static void init_empty_kv(struct config_kv *kv,
                          struct config_kv_entry *entries,
                          size_t capacity)
{
    memset(entries, 0, sizeof(entries[0]) * capacity);
    CHECK(config_kv_init(kv, entries, capacity) == CONFIG_KV_OK,
          "compaction fixture initializes kv");
}

static void populate_state(struct config_kv *kv,
                           struct config_kv_entry *entries,
                           bool reverse_slots)
{
    bool replaced = false;

    init_empty_kv(kv, entries, TEST_CAPACITY);
    if (!reverse_slots) {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                            "beta", 4U, "two", 3U, &replaced) ==
                  CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "one", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "alpha", 5U, "audit-one", 9U, &replaced) ==
                      CONFIG_KV_OK,
              "forward checkpoint populates exact entries");
    } else {
        CHECK(config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                            "alpha", 5U, "audit-one", 9U, &replaced) ==
                  CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "alpha", 5U, "one", 3U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_AUDIT,
                                "beta", 4U, "audit-two", 9U, &replaced) ==
                      CONFIG_KV_OK &&
                  config_kv_put(kv, CONFIG_KV_KIND_CONFIG,
                                "beta", 4U, "two", 3U, &replaced) ==
                      CONFIG_KV_OK,
              "reverse checkpoint populates exact entries");
    }
}

static bool candidate_entries_equal(
    const struct journal_compaction_candidate *left,
    const struct journal_compaction_candidate *right)
{
    return left->count == right->count &&
           memcmp(left->entries, right->entries,
                  sizeof(left->entries[0]) * left->count) == 0;
}

static bool candidate_selected(const struct journal_compaction_candidate *candidate,
                               uint64_t source_sequence)
{
    return candidate->valid &&
           candidate->count == 4U &&
           candidate->config_count == 2U &&
           candidate->audit_count == 2U &&
           candidate->source_sequence == source_sequence &&
           entry_equal(&candidate->entries[0], CONFIG_KV_KIND_CONFIG,
                       "beta", "two", 1U) &&
           entry_equal(&candidate->entries[1], CONFIG_KV_KIND_CONFIG,
                       "alpha", "one", 1U) &&
           entry_equal(&candidate->entries[2], CONFIG_KV_KIND_AUDIT,
                       "beta", "audit-two", 1U) &&
           entry_equal(&candidate->entries[3], CONFIG_KV_KIND_AUDIT,
                       "alpha", "audit-one", 1U);
}

static void make_candidate(
    struct journal_compaction_candidate *candidate,
    struct config_kv_entry *entries,
    bool reverse_slots,
    uint64_t sequence)
{
    struct config_kv kv;
    struct journal_compaction_checkpoint checkpoints[1];

    populate_state(&kv, entries, reverse_slots);
    checkpoints[0].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[0].sequence = sequence;
    checkpoints[0].kv = &kv;
    CHECK(journal_compaction_select(candidate, checkpoints, 1U) ==
              JOURNAL_COMPACTION_OK &&
              candidate->source_sequence == sequence,
          "compaction commit fixture publishes candidate");
}

static void copy_candidate_to_generation(
    struct journal_compaction_generation *generation,
    const struct journal_compaction_candidate *candidate,
    uint64_t generation_id,
    bool staged,
    bool interrupted)
{
    size_t index;

    memset(generation, 0, sizeof(*generation));
    generation->generation_id = generation_id;
    generation->source_sequence = candidate->source_sequence;
    for (index = 0U; index < candidate->count; ++index) {
        generation->entries[index] = candidate->entries[index];
    }
    generation->count = candidate->count;
    generation->config_count = candidate->config_count;
    generation->audit_count = candidate->audit_count;
    generation->valid = true;
    generation->active = false;
    generation->staged = staged;
    generation->interrupted = interrupted;
}

static bool generation_matches_candidate(
    const struct journal_compaction_generation *generation,
    const struct journal_compaction_candidate *candidate)
{
    return generation != NULL &&
           candidate != NULL &&
           generation->valid &&
           generation->source_sequence == candidate->source_sequence &&
           generation->count == candidate->count &&
           generation->config_count == candidate->config_count &&
           generation->audit_count == candidate->audit_count &&
           (generation->count == 0U ||
            memcmp(generation->entries, candidate->entries,
                   sizeof(candidate->entries[0]) * candidate->count) == 0);
}

static bool exactly_one_active_generation(
    const struct journal_compaction_commit *commit)
{
    size_t active_count = 0U;
    size_t index;

    if (commit == NULL) {
        return false;
    }
    for (index = 0U;
         index < JOURNAL_COMPACTION_GENERATION_SLOT_COUNT;
         ++index) {
        if (commit->slots[index].active) {
            ++active_count;
        }
    }
    return active_count == 1U &&
           commit->active_slot <
               JOURNAL_COMPACTION_GENERATION_SLOT_COUNT &&
           commit->slots[commit->active_slot].active &&
           commit->slots[commit->active_slot].valid;
}

static bool no_interrupted_generation(
    const struct journal_compaction_commit *commit)
{
    size_t index;

    if (commit == NULL) {
        return false;
    }
    for (index = 0U;
         index < JOURNAL_COMPACTION_GENERATION_SLOT_COUNT;
         ++index) {
        if (commit->slots[index].interrupted) {
            return false;
        }
    }
    return true;
}

struct fault_io {
    uint8_t fail_phase;
    uint32_t fail_after_writes;
    uint32_t writes;
};

static bool fault_write(
    void *context,
    uint8_t phase,
    size_t slot_index,
    const struct journal_compaction_generation *generation)
{
    struct fault_io *io = (struct fault_io *)context;

    (void)slot_index;
    (void)generation;
    ++io->writes;
    return !(io->fail_phase != 0xFFU &&
             phase == io->fail_phase &&
             io->writes > io->fail_after_writes);
}

static void init_fault_io(struct fault_io *io,
                          uint8_t fail_phase,
                          uint32_t fail_after_writes)
{
    io->fail_phase = fail_phase;
    io->fail_after_writes = fail_after_writes;
    io->writes = 0U;
}

static struct journal_compaction_io make_io(struct fault_io *io)
{
    struct journal_compaction_io result;

    result.context = io;
    result.write = fault_write;
    return result;
}

static void test_validation_and_status_strings(void) {
    static const enum journal_compaction_status statuses[] = {
        JOURNAL_COMPACTION_OK,
        JOURNAL_COMPACTION_NULL,
        JOURNAL_COMPACTION_BAD_STATE,
        JOURNAL_COMPACTION_BAD_CAPACITY,
        JOURNAL_COMPACTION_BAD_VERSION,
        JOURNAL_COMPACTION_BAD_SEQUENCE,
        JOURNAL_COMPACTION_DUPLICATE,
        JOURNAL_COMPACTION_BAD_GENERATION,
        JOURNAL_COMPACTION_BAD_SOURCE,
        JOURNAL_COMPACTION_WRITE_FAULT,
    };
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv kv;
    struct journal_compaction_checkpoint checkpoints[2];
    struct journal_compaction_candidate candidate;
    unsigned char before[sizeof(candidate)];
    size_t index;

    populate_state(&kv, entries, false);
    checkpoints[0].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[0].sequence = 1U;
    checkpoints[0].kv = &kv;
    checkpoints[1].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[1].sequence = 2U;
    checkpoints[1].kv = &kv;
    memset(&candidate, 0xCD, sizeof(candidate));

    CHECK(journal_compaction_select(NULL, checkpoints, 1U) ==
              JOURNAL_COMPACTION_NULL,
          "candidate select rejects null candidate");
    CHECK(journal_compaction_select(&candidate, NULL, 1U) ==
              JOURNAL_COMPACTION_NULL,
          "candidate select rejects null checkpoints with count");
    CHECK(journal_compaction_select(&candidate, checkpoints, 0U) ==
              JOURNAL_COMPACTION_BAD_CAPACITY,
          "candidate select rejects zero checkpoint count");
    CHECK(journal_compaction_select(
              &candidate, checkpoints,
              JOURNAL_COMPACTION_MAX_CHECKPOINTS + 1U) ==
              JOURNAL_COMPACTION_BAD_CAPACITY,
          "candidate select rejects over-limit checkpoint count");

    checkpoints[1].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION + 1U;
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_BAD_VERSION,
          "candidate select rejects unsupported checkpoint version");
    checkpoints[1].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;

    checkpoints[1].sequence = 0U;
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_BAD_SEQUENCE,
          "candidate select rejects zero checkpoint sequence");
    checkpoints[1].sequence = 2U;

    checkpoints[1].sequence = 1U;
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_DUPLICATE,
          "candidate select rejects duplicate checkpoint sequences");
    checkpoints[1].sequence = 2U;

    checkpoints[1].kv = NULL;
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_NULL,
          "candidate select rejects null checkpoint kv");
    checkpoints[1].kv = &kv;

    memset(&kv, 0, sizeof(kv));
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_BAD_STATE,
          "candidate select rejects uninitialized checkpoint kv");
    populate_state(&kv, entries, false);

    memcpy(before, &candidate, sizeof(before));
    checkpoints[1].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION + 1U;
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_BAD_VERSION &&
              memcmp(&candidate, before, sizeof(candidate)) == 0,
          "failed candidate select leaves destination unchanged");
    checkpoints[1].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(journal_compaction_status_string(statuses[index]) != NULL &&
                  strcmp(journal_compaction_status_string(statuses[index]),
                         statuses[index] == JOURNAL_COMPACTION_OK ?
                             "JOURNAL_COMPACTION_OK" :
                         statuses[index] == JOURNAL_COMPACTION_NULL ?
                             "JOURNAL_COMPACTION_NULL" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_STATE ?
                             "JOURNAL_COMPACTION_BAD_STATE" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_CAPACITY ?
                             "JOURNAL_COMPACTION_BAD_CAPACITY" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_VERSION ?
                             "JOURNAL_COMPACTION_BAD_VERSION" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_SEQUENCE ?
                             "JOURNAL_COMPACTION_BAD_SEQUENCE" :
                         statuses[index] ==
                                 JOURNAL_COMPACTION_BAD_GENERATION ?
                             "JOURNAL_COMPACTION_BAD_GENERATION" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_SOURCE ?
                             "JOURNAL_COMPACTION_BAD_SOURCE" :
                         statuses[index] == JOURNAL_COMPACTION_WRITE_FAULT ?
                             "JOURNAL_COMPACTION_WRITE_FAULT" :
                             "JOURNAL_COMPACTION_DUPLICATE") == 0,
              "journal-compaction status string is exact");
    }
    CHECK(strcmp(journal_compaction_status_string(
                     (enum journal_compaction_status)99U),
                 "JOURNAL_COMPACTION_UNKNOWN") == 0,
          "journal-compaction undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate validation performs no allocation");
}

static void test_deterministic_selection(void) {
    struct config_kv_entry forward_entries[TEST_CAPACITY];
    struct config_kv_entry reverse_entries[TEST_CAPACITY];
    struct config_kv forward;
    struct config_kv reverse;
    struct journal_compaction_checkpoint checkpoints[2];
    struct journal_compaction_candidate candidate;
    struct journal_compaction_candidate reverse_candidate;

    populate_state(&forward, forward_entries, false);
    populate_state(&reverse, reverse_entries, true);

    checkpoints[0].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[0].sequence = 1U;
    checkpoints[0].kv = &forward;
    checkpoints[1].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[1].sequence = 2U;
    checkpoints[1].kv = &forward;

    memset(&candidate, 0, sizeof(candidate));
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_OK &&
              candidate_selected(&candidate, 2U),
          "candidate select publishes newest committed checkpoint exactly");

    checkpoints[1].kv = &reverse;
    memset(&reverse_candidate, 0, sizeof(reverse_candidate));
    CHECK(journal_compaction_select(&reverse_candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_OK &&
              candidate_entries_equal(&candidate, &reverse_candidate) &&
              reverse_candidate.source_sequence == 2U,
          "equal checkpoints select byte-identical entries across slot order");

    checkpoints[0].sequence = 7U;
    checkpoints[1].sequence = 3U;
    memset(&candidate, 0, sizeof(candidate));
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_OK &&
              candidate.source_sequence == 7U &&
              candidate_selected(&candidate, 7U),
          "candidate select is independent of checkpoint-array order");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate selection performs no allocation");
}

static void test_failure_preserves_active_state(void) {
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry bad_entries[TEST_CAPACITY];
    struct config_kv active;
    struct config_kv bad;
    struct journal_compaction_checkpoint checkpoints[2];
    struct journal_compaction_candidate candidate;
    unsigned char candidate_before[sizeof(candidate)];
    unsigned char value[CONFIG_KV_VALUE_MAX];
    size_t value_length = 0U;
    uint64_t revision = 0U;

    populate_state(&active, active_entries, false);
    init_empty_kv(&bad, bad_entries, TEST_CAPACITY);
    bad.entries[0].occupied = true;
    bad.entries[0].kind = CONFIG_KV_KIND_COUNT;
    bad.count = 1U;

    checkpoints[0].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[0].sequence = 1U;
    checkpoints[0].kv = &active;
    checkpoints[1].version = JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    checkpoints[1].sequence = 2U;
    checkpoints[1].kv = &bad;

    memset(&candidate, 0xCD, sizeof(candidate));
    memcpy(candidate_before, &candidate, sizeof(candidate));
    CHECK(journal_compaction_select(&candidate, checkpoints, 2U) ==
              JOURNAL_COMPACTION_BAD_STATE,
          "corrupt committed checkpoint fails candidate selection");
    CHECK(memcmp(&candidate, candidate_before, sizeof(candidate)) == 0,
          "candidate failure leaves candidate bytes unchanged");
    CHECK(config_kv_get(&active, CONFIG_KV_KIND_CONFIG,
                        "beta", 4U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_OK &&
              value_length == 3U &&
              bytes_equal(value, (const unsigned char *)"two", 3U),
          "candidate failure leaves active config record readable");
    CHECK(config_kv_get(&active, CONFIG_KV_KIND_AUDIT,
                        "alpha", 5U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_OK &&
              value_length == 9U &&
              bytes_equal(value, (const unsigned char *)"audit-one", 9U),
          "candidate failure leaves active audit record readable");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate failure performs no allocation");
}

static void test_commit_validation_and_status_strings(void) {
    static const enum journal_compaction_status statuses[] = {
        JOURNAL_COMPACTION_OK,
        JOURNAL_COMPACTION_NULL,
        JOURNAL_COMPACTION_BAD_STATE,
        JOURNAL_COMPACTION_BAD_CAPACITY,
        JOURNAL_COMPACTION_BAD_VERSION,
        JOURNAL_COMPACTION_BAD_SEQUENCE,
        JOURNAL_COMPACTION_DUPLICATE,
        JOURNAL_COMPACTION_BAD_GENERATION,
        JOURNAL_COMPACTION_BAD_SOURCE,
        JOURNAL_COMPACTION_WRITE_FAULT,
    };
    struct config_kv_entry entries[TEST_CAPACITY];
    struct journal_compaction_candidate candidate;
    struct journal_compaction_candidate invalid;
    struct journal_compaction_commit commit;
    struct journal_compaction_commit uninitialized;
    struct fault_io io_state;
    struct journal_compaction_io io;
    size_t index;

    memset(&invalid, 0, sizeof(invalid));
    CHECK(journal_compaction_commit_init(NULL, &candidate) ==
              JOURNAL_COMPACTION_NULL,
          "commit init rejects null commit");
    CHECK(journal_compaction_commit_init(&commit, NULL) ==
              JOURNAL_COMPACTION_NULL,
          "commit init rejects null initial candidate");
    CHECK(journal_compaction_commit_init(&commit, &invalid) ==
              JOURNAL_COMPACTION_BAD_STATE,
          "commit init rejects invalid initial candidate");

    make_candidate(&candidate, entries, false, 1U);
    memset(&commit, 0, sizeof(commit));
    CHECK(journal_compaction_commit_init(&commit, &candidate) ==
              JOURNAL_COMPACTION_OK &&
              exactly_one_active_generation(&commit),
          "commit init publishes exactly one valid active generation");
    CHECK(journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id ==
                  JOURNAL_COMPACTION_INITIAL_GENERATION &&
              generation_matches_candidate(
                  journal_compaction_active(&commit), &candidate),
          "commit init active generation matches initial candidate");
    CHECK(journal_compaction_commit_init(&commit, &candidate) ==
              JOURNAL_COMPACTION_BAD_GENERATION,
          "commit init rejects reinitialization");

    init_fault_io(&io_state, 0xFFU, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_publish(NULL, &candidate, &io) ==
              JOURNAL_COMPACTION_NULL,
          "commit publish rejects null commit");
    CHECK(journal_compaction_publish(&commit, NULL, &io) ==
              JOURNAL_COMPACTION_NULL,
          "commit publish rejects null candidate");
    CHECK(journal_compaction_publish(&commit, &candidate, NULL) ==
              JOURNAL_COMPACTION_NULL,
          "commit publish rejects null io");
    memset(&uninitialized, 0, sizeof(uninitialized));
    CHECK(journal_compaction_publish(&uninitialized, &candidate, &io) ==
              JOURNAL_COMPACTION_BAD_GENERATION,
          "commit publish rejects uninitialized commit");
    memset(&invalid, 0, sizeof(invalid));
    CHECK(journal_compaction_publish(&commit, &invalid, &io) ==
              JOURNAL_COMPACTION_BAD_STATE,
          "commit publish rejects invalid candidate");
    CHECK(journal_compaction_recover(NULL, &io) ==
              JOURNAL_COMPACTION_NULL,
          "commit recover rejects null commit");
    CHECK(journal_compaction_recover(&commit, NULL) ==
              JOURNAL_COMPACTION_NULL,
          "commit recover rejects null io");
    CHECK(journal_compaction_recover(&uninitialized, &io) ==
              JOURNAL_COMPACTION_BAD_GENERATION,
          "commit recover rejects uninitialized commit");
    CHECK(journal_compaction_active(NULL) == NULL,
          "active accessor rejects null commit");

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(journal_compaction_status_string(statuses[index]) != NULL &&
                  strcmp(journal_compaction_status_string(statuses[index]),
                         statuses[index] == JOURNAL_COMPACTION_OK ?
                             "JOURNAL_COMPACTION_OK" :
                         statuses[index] == JOURNAL_COMPACTION_NULL ?
                             "JOURNAL_COMPACTION_NULL" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_STATE ?
                             "JOURNAL_COMPACTION_BAD_STATE" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_CAPACITY ?
                             "JOURNAL_COMPACTION_BAD_CAPACITY" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_VERSION ?
                             "JOURNAL_COMPACTION_BAD_VERSION" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_SEQUENCE ?
                             "JOURNAL_COMPACTION_BAD_SEQUENCE" :
                         statuses[index] == JOURNAL_COMPACTION_DUPLICATE ?
                             "JOURNAL_COMPACTION_DUPLICATE" :
                         statuses[index] ==
                                 JOURNAL_COMPACTION_BAD_GENERATION ?
                             "JOURNAL_COMPACTION_BAD_GENERATION" :
                         statuses[index] == JOURNAL_COMPACTION_BAD_SOURCE ?
                             "JOURNAL_COMPACTION_BAD_SOURCE" :
                         statuses[index] == JOURNAL_COMPACTION_WRITE_FAULT ?
                             "JOURNAL_COMPACTION_WRITE_FAULT" :
                             "JOURNAL_COMPACTION_UNKNOWN") == 0,
              "journal-compaction commit status string is exact");
    }
    CHECK(strcmp(journal_compaction_status_string(
                     (enum journal_compaction_status)99U),
                 "JOURNAL_COMPACTION_UNKNOWN") == 0,
          "journal-compaction undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "commit validation performs no allocation");
}

static void test_atomic_publication(void) {
    struct config_kv_entry first_entries[TEST_CAPACITY];
    struct config_kv_entry second_entries[TEST_CAPACITY];
    struct journal_compaction_candidate first;
    struct journal_compaction_candidate second;
    struct journal_compaction_commit commit;
    struct fault_io io_state;
    struct journal_compaction_io io;

    make_candidate(&first, first_entries, false, 1U);
    make_candidate(&second, second_entries, true, 2U);
    memset(&commit, 0, sizeof(commit));
    CHECK(journal_compaction_commit_init(&commit, &first) ==
              JOURNAL_COMPACTION_OK,
          "atomic publication initializes commit");

    init_fault_io(&io_state, 0xFFU, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_OK &&
              exactly_one_active_generation(&commit),
          "atomic publication advances to exactly one active generation");
    CHECK(journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 2U &&
              generation_matches_candidate(
                  journal_compaction_active(&commit), &second),
          "atomic publication active generation matches candidate");
    CHECK(!commit.slots[0].active && commit.slots[0].valid &&
              no_interrupted_generation(&commit),
          "atomic publication retains previous inactive generation without interruption");
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_BAD_SOURCE,
          "atomic publication rejects stale candidate");
    CHECK(exactly_one_active_generation(&commit) &&
              journal_compaction_active(&commit)->generation_id == 2U,
          "stale publication leaves active generation unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "atomic publication performs no allocation");
}

static void test_phase_zero_failure_rolls_back(void) {
    struct config_kv_entry first_entries[TEST_CAPACITY];
    struct config_kv_entry second_entries[TEST_CAPACITY];
    struct journal_compaction_candidate first;
    struct journal_compaction_candidate second;
    struct journal_compaction_commit commit;
    struct fault_io io_state;
    struct journal_compaction_io io;

    make_candidate(&first, first_entries, false, 1U);
    make_candidate(&second, second_entries, true, 2U);
    memset(&commit, 0, sizeof(commit));
    CHECK(journal_compaction_commit_init(&commit, &first) ==
              JOURNAL_COMPACTION_OK,
          "phase-zero fixture initializes commit");

    init_fault_io(&io_state, 0U, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_WRITE_FAULT,
          "phase-zero failure returns write fault");
    CHECK(exactly_one_active_generation(&commit) &&
              journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 1U &&
              generation_matches_candidate(
                  journal_compaction_active(&commit), &first),
          "phase-zero failure keeps previous active generation");
    CHECK(commit.slots[1].interrupted && !commit.slots[1].staged,
          "phase-zero failure marks unstaged interrupted slot");

    init_fault_io(&io_state, 0xFFU, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_recover(&commit, &io) ==
              JOURNAL_COMPACTION_OK &&
              exactly_one_active_generation(&commit) &&
              no_interrupted_generation(&commit),
          "phase-zero recovery discards interrupted slot and keeps active generation");
    CHECK(journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 1U &&
              generation_matches_candidate(
                  journal_compaction_active(&commit), &first),
          "phase-zero recovery active generation remains readable");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "phase-zero recovery performs no allocation");
}

static void test_phase_one_failure_recovers_staged(void) {
    struct config_kv_entry first_entries[TEST_CAPACITY];
    struct config_kv_entry second_entries[TEST_CAPACITY];
    struct journal_compaction_candidate first;
    struct journal_compaction_candidate second;
    struct journal_compaction_commit commit;
    struct fault_io io_state;
    struct journal_compaction_io io;

    make_candidate(&first, first_entries, false, 1U);
    make_candidate(&second, second_entries, true, 2U);
    memset(&commit, 0, sizeof(commit));
    CHECK(journal_compaction_commit_init(&commit, &first) ==
              JOURNAL_COMPACTION_OK,
          "phase-one fixture initializes commit");

    init_fault_io(&io_state, 1U, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_WRITE_FAULT,
          "phase-one failure returns write fault");
    CHECK(exactly_one_active_generation(&commit) &&
              journal_compaction_active(&commit)->generation_id == 1U,
          "phase-one failure keeps previous active generation");
    CHECK(commit.slots[1].interrupted && commit.slots[1].staged &&
              commit.slots[1].source_sequence == 2U,
          "phase-one failure marks staged interrupted generation");

    init_fault_io(&io_state, 0xFFU, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_BAD_GENERATION,
          "publish rejects interrupted commit state before recovery");

    CHECK(journal_compaction_recover(&commit, &io) ==
              JOURNAL_COMPACTION_OK &&
              exactly_one_active_generation(&commit) &&
              no_interrupted_generation(&commit),
          "recovery retry publishes staged generation as only active");
    CHECK(journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 2U &&
              generation_matches_candidate(
                  journal_compaction_active(&commit), &second),
          "recovery active generation matches staged candidate");
    CHECK(journal_compaction_recover(&commit, &io) ==
              JOURNAL_COMPACTION_OK &&
              journal_compaction_active(&commit)->generation_id == 2U &&
              exactly_one_active_generation(&commit),
          "recovery without interruption is idempotent");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "phase-one recovery performs no allocation");
}

static void test_phase_one_recovery_failure_keeps_active(void) {
    struct config_kv_entry first_entries[TEST_CAPACITY];
    struct config_kv_entry second_entries[TEST_CAPACITY];
    struct journal_compaction_candidate first;
    struct journal_compaction_candidate second;
    struct journal_compaction_commit commit;
    struct fault_io io_state;
    struct journal_compaction_io io;

    make_candidate(&first, first_entries, false, 1U);
    make_candidate(&second, second_entries, true, 2U);
    memset(&commit, 0, sizeof(commit));
    CHECK(journal_compaction_commit_init(&commit, &first) ==
              JOURNAL_COMPACTION_OK,
          "recovery-failure fixture initializes commit");

    init_fault_io(&io_state, 1U, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_publish(&commit, &second, &io) ==
              JOURNAL_COMPACTION_WRITE_FAULT,
          "recovery-failure fixture interrupts phase one");

    init_fault_io(&io_state, 1U, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_recover(&commit, &io) ==
              JOURNAL_COMPACTION_OK &&
              exactly_one_active_generation(&commit) &&
              no_interrupted_generation(&commit),
          "failed recovery retry keeps exactly one active generation");
    CHECK(journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 1U &&
              generation_matches_candidate(
                  journal_compaction_active(&commit), &first),
          "failed recovery retry keeps previous active generation readable");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed recovery retry performs no allocation");
}

static void test_stale_interrupted_generation_not_promoted(void) {
    struct config_kv_entry first_entries[TEST_CAPACITY];
    struct config_kv_entry second_entries[TEST_CAPACITY];
    struct journal_compaction_candidate first;
    struct journal_compaction_candidate second;
    struct journal_compaction_commit commit;
    struct fault_io io_state;
    struct journal_compaction_io io;

    make_candidate(&first, first_entries, false, 1U);
    make_candidate(&second, second_entries, true, 2U);
    memset(&commit, 0, sizeof(commit));
    CHECK(journal_compaction_commit_init(&commit, &second) ==
              JOURNAL_COMPACTION_OK,
          "stale-interruption fixture initializes newer active generation");

    copy_candidate_to_generation(&commit.slots[1], &first, 2U, true, true);
    init_fault_io(&io_state, 0xFFU, 0U);
    io = make_io(&io_state);
    CHECK(journal_compaction_recover(&commit, &io) ==
              JOURNAL_COMPACTION_OK &&
              exactly_one_active_generation(&commit) &&
              no_interrupted_generation(&commit),
          "stale interrupted generation is discarded by recovery");
    CHECK(journal_compaction_active(&commit) != NULL &&
              journal_compaction_active(&commit)->generation_id == 1U &&
              generation_matches_candidate(
                  journal_compaction_active(&commit), &second),
          "stale interrupted generation cannot become active");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "stale interruption recovery performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_deterministic_selection();
    test_failure_preserves_active_state();
    test_commit_validation_and_status_strings();
    test_atomic_publication();
    test_phase_zero_failure_rolls_back();
    test_phase_one_failure_recovers_staged();
    test_phase_one_recovery_failure_keeps_active();
    test_stale_interrupted_generation_not_promoted();

    if (failures != 0) {
        fprintf(stderr, "Journal compaction host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Journal compaction host tests: ALL OK\n");
    return 0;
}

#endif
