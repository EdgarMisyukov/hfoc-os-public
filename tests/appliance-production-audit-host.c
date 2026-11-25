#include "appliance_production_audit_boot.h"

#include "appliance_audit_chain.h"
#include "appliance_audit_export.h"
#include "audit.h"
#include "event_ring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_AUDIT_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct event_ring ring;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct kernel_event audit_event(
    uint32_t type,
    uint32_t operation,
    uint64_t detail)
{
    struct kernel_event event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.flags = AUDIT_EVENT_FLAG_STRUCTURED;
    event.data[0] = (uint64_t)type |
                    ((uint64_t)operation << 8U);
    event.data[1] = detail;
    return event;
}

static bool push_event(const struct kernel_event *event)
{
    return event_ring_push(&ring, event) == EVENT_RING_OK;
}

static bool push_audit(
    uint32_t sequence,
    uint32_t type,
    uint32_t operation,
    uint64_t detail)
{
    struct kernel_event event = audit_event(type, operation, detail);

    event.sequence = sequence;
    return push_event(&event);
}

static void reset_ring(void)
{
    memset(&ring, 0, sizeof(ring));
    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "fixture ring initializes");
}

static void test_state_strings(void)
{
    CHECK(strcmp(appliance_production_audit_boot_state_string(
                     APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY),
                 "READY") == 0,
          "state string for READY");
    CHECK(appliance_production_audit_boot_state_string(
              (enum appliance_production_audit_boot_state)99) == NULL,
          "unknown state string is null");
}

static void test_unavailable_without_ring(void)
{
    struct appliance_production_audit_view view;
    char rendered[APPLIANCE_PRODUCTION_AUDIT_BOOT_RENDER_MAX + 1U];
    size_t rendered_length = 0U;
    struct event_ring unpublished;

    memset(&unpublished, 0, sizeof(unpublished));
    CHECK(!appliance_production_audit_boot_init(
              &unpublished, 1U, 1U, 0U),
          "an unpublished ring fails the boot composition");
    CHECK(appliance_production_audit_boot_state() ==
              APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_UNAVAILABLE &&
              !appliance_production_audit_boot_available(),
          "a failed composition publishes UNAVAILABLE");
    CHECK(appliance_production_audit_boot_view(&view) &&
              view.published &&
              view.state ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_UNAVAILABLE &&
              view.records == 0U,
          "the unavailable view is still published honestly");
    CHECK(appliance_production_audit_boot_render(
              rendered, sizeof(rendered), &rendered_length) &&
              strstr(rendered,
                     "audit_chain v=1 state=UNAVAILABLE "
                     "verify=UNAVAILABLE tamper=0 skipped=0 records=0 "
                     "export=NONE") != NULL,
          "the unavailable view renders the exact reason");
    CHECK(appliance_production_audit_boot_init(
              NULL, 1U, 1U, 0U) == false &&
              appliance_production_audit_boot_init(
                  &unpublished, 0U, 1U, 0U) == false &&
              appliance_production_audit_boot_init(
                  &unpublished, 1U, 0x100U, 0U) == false,
          "the composition refuses invalid boot arguments");
}

static void test_ready_boot_chain(void)
{
    struct appliance_production_audit_view view;
    struct appliance_audit_chain_verdict verdict;
    const struct appliance_audit_export_cursor *cursor;
    char rendered[APPLIANCE_PRODUCTION_AUDIT_BOOT_RENDER_MAX + 1U];
    size_t rendered_length = 0U;

    reset_ring();
    CHECK(push_audit(1U, AUDIT_RECORD_NETWORK, 3U, 0x11U) &&
              push_audit(2U, AUDIT_RECORD_NETWORK, 4U, 0x22U),
          "fixture pushes two structured audit records");
    CHECK(appliance_production_audit_boot_init(
              &ring, 1U, 1U, 0x33U),
          "the boot composition verifies and exports");
    CHECK(appliance_production_audit_boot_available() &&
              appliance_production_audit_boot_state() ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY,
          "a clean boot publishes READY");
    CHECK(appliance_production_audit_boot_view(&view) &&
              view.state ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY &&
              view.verify_ok && !view.tamper_detected &&
              !view.rotated && view.skipped == 0U &&
              view.records == 3U &&
              view.first_sequence == 1U &&
              view.next_sequence == 4U &&
              view.exported == 3U,
          "the boot view counts the boot record and both ring records");
    CHECK(appliance_audit_chain_verify(
              appliance_production_audit_boot_chain(), &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              verdict.chain_ok && verdict.checked_count == 3U,
          "the published chain verifies over all three records");
    cursor = appliance_production_audit_boot_export();
    CHECK(cursor != NULL &&
              cursor->active && cursor->exported_count == 3U &&
              cursor->ack_count == 3U &&
              cursor->last_ack_sequence == 3U &&
              cursor->next_sequence == 4U &&
              cursor->gap_count == 0U,
          "the boot export acknowledged every chained record");
    CHECK(appliance_production_audit_boot_last_status() ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_production_audit_boot_last_chain_status() ==
                  APPLIANCE_AUDIT_CHAIN_OK,
          "the boot composition publishes its exact statuses");
    CHECK(appliance_production_audit_boot_render(
              rendered, sizeof(rendered), &rendered_length) &&
              strstr(rendered,
                     "audit_chain v=1 state=READY verify=OK tamper=0 "
                     "skipped=0 audit_chain v=1 generation=1 state=OK "
                     "records=3 first=1 next=4 capacity=32 appended=3 "
                     "rotated=0 anchored=NO audit_export v=1 "
                     "state=COMPLETE generation=1 first=1 next=4 end=4 "
                     "ack=3 exported=3 acked=3 gap=0") != NULL,
          "the boot view renders one canonical line");
    CHECK(!appliance_production_audit_boot_render(
              rendered, 16U, &rendered_length) &&
              !appliance_production_audit_boot_render(
                  NULL, sizeof(rendered), &rendered_length) &&
              !appliance_production_audit_boot_view(NULL),
          "the boot render fails closed on bad arguments");
}

static void test_non_audit_events_are_ignored(void)
{
    struct kernel_event plain;
    struct appliance_production_audit_view view;

    reset_ring();
    memset(&plain, 0, sizeof(plain));
    plain.type = 0x1234U;
    plain.flags = 0U;
    plain.data[0] = 0xAAU;
    CHECK(push_event(&plain) &&
              push_audit(1U, AUDIT_RECORD_SYSCALL, 2U, 0x44U),
          "fixture pushes a non-audit and an audit event");
    CHECK(appliance_production_audit_boot_init(
              &ring, 2U, 1U, 0x55U) &&
              appliance_production_audit_boot_state() ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY &&
              appliance_production_audit_boot_view(&view) &&
              view.records == 2U && view.skipped == 0U,
          "non-audit events are not counted as audit records");
}

static void test_corrupt_audit_event_degrades(void)
{
    struct kernel_event corrupt;
    struct appliance_production_audit_view view;
    char rendered[APPLIANCE_PRODUCTION_AUDIT_BOOT_RENDER_MAX + 1U];
    size_t rendered_length = 0U;

    reset_ring();
    corrupt = audit_event(AUDIT_RECORD_NETWORK, 1U, 0x66U);
    corrupt.data[2] = 0xFFFFFFFFU;
    CHECK(push_event(&corrupt) &&
              push_audit(1U, AUDIT_RECORD_TRUST, 2U, 0x77U),
          "fixture pushes a corrupt structured audit event");
    CHECK(appliance_production_audit_boot_init(
              &ring, 3U, 1U, 0x88U) &&
              appliance_production_audit_boot_state() ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_DEGRADED &&
              appliance_production_audit_boot_available() &&
              appliance_production_audit_boot_view(&view) &&
              view.skipped == 1U && view.records == 2U &&
              view.verify_ok,
          "a rejected structured audit event degrades the view");
    CHECK(appliance_production_audit_boot_render(
              rendered, sizeof(rendered), &rendered_length) &&
              strstr(rendered,
                     "state=DEGRADED verify=OK tamper=0 skipped=1") != NULL,
          "the degraded view publishes the skipped count");
}

static void test_retention_rotation_degrades(void)
{
    struct appliance_production_audit_view view;
    struct appliance_audit_chain_verdict verdict;
    uint32_t index;

    reset_ring();
    for (index = 0U; index < APPLIANCE_AUDIT_CHAIN_CAPACITY; ++index) {
        if (!push_audit(index + 1U, AUDIT_RECORD_PROCESS, 1U,
                        index)) {
            break;
        }
    }
    CHECK(event_ring_count(&ring) == APPLIANCE_AUDIT_CHAIN_CAPACITY,
          "fixture fills the chain window exactly");
    CHECK(appliance_production_audit_boot_init(
              &ring, 4U, 1U, 0U),
          "a full ring still composes and exports");
    CHECK(appliance_production_audit_boot_view(&view) &&
              view.rotated && view.rotated_count ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_ROTATE_BATCH &&
              view.records ==
                  APPLIANCE_AUDIT_CHAIN_CAPACITY -
                      APPLIANCE_PRODUCTION_AUDIT_BOOT_ROTATE_BATCH +
                      1U &&
              view.state ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_DEGRADED,
          "retention rotation is reported as DEGRADED");
    CHECK(appliance_audit_chain_verify(
              appliance_production_audit_boot_chain(), &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              verdict.chain_ok && verdict.anchored &&
              verdict.first_sequence ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_ROTATE_BATCH + 1U,
          "the rotated boot chain still verifies against its anchor");
}

static void test_render_reverifies_the_window(void)
{
    struct appliance_production_audit_view view;
    struct appliance_audit_chain *chain;
    char rendered[APPLIANCE_PRODUCTION_AUDIT_BOOT_RENDER_MAX + 1U];
    size_t rendered_length = 0U;

    reset_ring();
    CHECK(push_audit(1U, AUDIT_RECORD_NETWORK, 1U, 0xAAU) &&
              appliance_production_audit_boot_init(
                  &ring, 6U, 1U, 0U) &&
              appliance_production_audit_boot_state() ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY,
          "re-verification fixture starts READY");
    chain = (struct appliance_audit_chain *)
        appliance_production_audit_boot_chain();
    CHECK(chain != NULL &&
              appliance_audit_chain_host_test_tamper(chain, 0U),
          "the fixture rewrites the published window");
    CHECK(appliance_production_audit_boot_view(&view) &&
              view.state ==
                  APPLIANCE_PRODUCTION_AUDIT_BOOT_STATE_READY,
          "the frozen boot view still records the original decision");
    CHECK(appliance_production_audit_boot_render(
              rendered, sizeof(rendered), &rendered_length) &&
              strstr(rendered,
                     "audit_chain v=1 state=TAMPERED "
                     "verify=TAMPERED tamper=1 skipped=0") != NULL &&
              strstr(rendered, "audit_chain v=1 generation=6 "
                               "state=TAMPERED") != NULL,
          "the published line re-verifies and never claims READY");
}

static void test_no_allocation(void)
{
    reset_ring();
    CHECK(push_audit(1U, AUDIT_RECORD_NETWORK, 1U, 0x99U),
          "no-allocation fixture pushes a record");
    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(appliance_production_audit_boot_init(&ring, 5U, 1U, 0U),
          "no-allocation boot composition completes");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "the production audit composition never allocates");
}

int main(void)
{
    test_state_strings();
    test_unavailable_without_ring();
    test_ready_boot_chain();
    test_non_audit_events_are_ignored();
    test_corrupt_audit_event_degrades();
    test_retention_rotation_degrades();
    test_render_reverifies_the_window();
    test_no_allocation();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance production audit host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance production audit host tests: ALL OK");
    return 0;
}

#endif
