/*
 * ITEM-562 host proof: two controllers, both scan orders, production code.
 *
 * The owner's laptop has two Intel USB 2 functions.  The run after ITEM-561
 * printed `ehci=DESCRIPTOR_FAILED/NO_DEVICE` and a start snapshot of
 * `pci=00:1D.0`, while an earlier run had shown `00:1A.0`: the driver kept one
 * global verdict, which every controller overwrote, and one start snapshot,
 * which every start overwrote.  This proof runs the aggregation the driver
 * runs (kernel/src/ehci_verdict.c) with both controllers in both orders and
 * requires that:
 *
 *   * each controller keeps its own status, detail and walk stage;
 *   * a specific refusal is never replaced by a generic one;
 *   * the machine's answer is the first concrete refusal in scan order, not
 *     the last controller scanned;
 *   * a bound medium on either controller wins over every refusal;
 *   * the rendered per-controller list keeps both, whole, inside its bound.
 */
#include "ehci.h"
#include "ehci_verdict.h"
#include "ehci_walk.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

/* The walk report a controller that failed at a root port reset produces. */
static struct ehci_walk_report report_root_reset(void) {
    struct ehci_walk_report report;

    memset(&report, 0, sizeof(report));
    report.ports_connected = 1U;
    report.failures = 1U;
    report.port_count = 1U;
    report.ports[0].port = 1U;
    report.ports[0].outcome = EHCI_WALK_OUTCOME_PORT_RESET;
    report.ports[0].stage = "ROOT_RESET";
    report.outcome = EHCI_WALK_OUTCOME_PORT_RESET;
    report.failure = "ROOT_RESET";
    report.failure_port = 1U;
    return report;
}

/* A controller whose walk read a descriptor and failed there. */
static struct ehci_walk_report report_devices8(const char *detail) {
    struct ehci_walk_report report;

    memset(&report, 0, sizeof(report));
    report.ports_connected = 1U;
    report.failures = 1U;
    report.port_count = 1U;
    report.ports[0].port = 1U;
    report.ports[0].outcome = EHCI_WALK_OUTCOME_TRANSFER;
    report.ports[0].stage = "DEVICE8";
    report.ports[0].detail = detail;
    report.outcome = EHCI_WALK_OUTCOME_TRANSFER;
    report.failure = "DEVICE8";
    report.failure_detail = detail;
    report.failure_port = 1U;
    return report;
}

/* A controller with no device on any port. */
static struct ehci_walk_report report_empty(void) {
    struct ehci_walk_report report;

    memset(&report, 0, sizeof(report));
    report.outcome = EHCI_WALK_OUTCOME_NO_PORT;
    report.failure = "NO_CONNECTED_PORT";
    return report;
}

/* A controller that carried its medium and bound it. */
static struct ehci_walk_report report_bound(void) {
    struct ehci_walk_report report;

    memset(&report, 0, sizeof(report));
    report.ports_connected = 1U;
    report.attached = 1U;
    report.probes = 1U;
    report.selected = 1;
    report.failures = 1U;
    report.port_count = 1U;
    report.ports[0].port = 1U;
    report.ports[0].outcome = EHCI_WALK_OUTCOME_OK;
    report.ports[0].stage = "MEDIUM";
    report.outcome = EHCI_WALK_OUTCOME_OK;
    return report;
}

static void fill_entry(struct ehci_verdict_entry *entry, uint8_t device_id,
                       enum ehci_status status, const char *detail,
                       const struct ehci_walk_report *report, bool bound) {
    ehci_verdict_fill(entry, 0U, device_id, 0U, true, status, detail, 2U,
                      report, bound);
}

static void test_refusal_preserved_with_medium_on_the_second(void) {
    struct ehci_verdict_entry entries[2];
    struct ehci_verdict_choice choice;
    struct ehci_walk_report reset = report_root_reset();
    struct ehci_walk_report bound = report_bound();
    char text[EHCI_VERDICT_TEXT_MAX];
    size_t length;

    /* 00:1A.0 refuses at a root port reset, 00:1D.0 carries the boot stick. */
    fill_entry(&entries[0], 0x1AU, EHCI_STATUS_OK, "READY", &reset, false);
    fill_entry(&entries[1], 0x1DU, EHCI_STATUS_OK, "READY", &bound, true);
    ehci_verdict_choose(entries, 2U, &choice);

    CHECK(choice.index == 1 && choice.storage_bound,
          "the controller that bound the medium answers for the machine");
    CHECK(choice.status == EHCI_STATUS_OK &&
              strcmp(choice.detail, "STORAGE_BOUND") == 0,
          "a bound medium is the machine's verdict");
    CHECK(ehci_walk_outcome_status(entries[0].outcome) ==
              EHCI_STATUS_PORT_NOT_ENABLED,
          "the refused controller's own status is the port status");
    CHECK(entries[0].outcome == EHCI_WALK_OUTCOME_PORT_RESET &&
              strcmp(entries[0].failure, "ROOT_RESET") == 0,
          "the refused controller keeps its own walk stage");

    length = ehci_verdict_render(entries, 2U, text, sizeof(text));
    CHECK(length == strlen(text) && length < sizeof(text),
          "the per-controller list is a terminated bounded string");
    CHECK(strstr(text, "00:1A.0=PORT_NOT_ENABLED/ROOT_RESET") != NULL,
          "the list names the refused controller and its refusal");
    CHECK(strstr(text, "00:1D.0=OK/STORAGE_BOUND") != NULL,
          "the list names the controller that bound the medium");
}

static void test_the_same_machine_in_the_other_order(void) {
    struct ehci_verdict_entry entries[2];
    struct ehci_verdict_choice choice;
    struct ehci_walk_report bound = report_bound();
    struct ehci_walk_report reset = report_root_reset();

    /* 00:1A.0 carries the stick, 00:1D.0 refuses: the scan order is the only
       difference, and the answer must not depend on it. */
    fill_entry(&entries[0], 0x1AU, EHCI_STATUS_OK, "READY", &bound, true);
    fill_entry(&entries[1], 0x1DU, EHCI_STATUS_OK, "READY", &reset, false);
    ehci_verdict_choose(entries, 2U, &choice);

    CHECK(choice.index == 0 && choice.storage_bound && choice.status ==
              EHCI_STATUS_OK,
          "the bound medium answers in either scan order");
    CHECK(entries[1].outcome == EHCI_WALK_OUTCOME_PORT_RESET,
          "the refused controller still keeps its stage");
}

static void test_nothing_bound_names_the_first_concrete_refusal(void) {
    struct ehci_verdict_entry entries[2];
    struct ehci_verdict_choice choice;
    struct ehci_walk_report devices = report_devices8("result=XACTERR");
    struct ehci_walk_report empty = report_empty();

    /* This is the run the owner reported, except that the first controller's
       walk reached a descriptor read: the machine must answer with it, not
       with the empty controller that happened to be scanned last. */
    fill_entry(&entries[0], 0x1AU, EHCI_STATUS_OK, "READY", &devices, false);
    fill_entry(&entries[1], 0x1DU, EHCI_STATUS_OK, "READY", &empty, false);
    ehci_verdict_choose(entries, 2U, &choice);

    CHECK(choice.index == 0, "the first concrete refusal answers");
    CHECK(choice.status == EHCI_STATUS_DESCRIPTOR_FAILED,
          "a descriptor refusal keeps the descriptor status");
    CHECK(strcmp(choice.detail, "DEVICE8") == 0,
          "the answer names the stage, not NO_DEVICE");
    CHECK(!choice.storage_bound, "nothing was bound");

    /* And the reverse order does not turn a refusal into an empty machine. */
    fill_entry(&entries[0], 0x1AU, EHCI_STATUS_OK, "READY", &empty, false);
    fill_entry(&entries[1], 0x1DU, EHCI_STATUS_OK, "READY", &devices, false);
    ehci_verdict_choose(entries, 2U, &choice);
    CHECK(choice.index == 1 && choice.status == EHCI_STATUS_DESCRIPTOR_FAILED,
          "the walk that got further answers in either scan order");
}

static void test_a_generic_answer_never_replaces_a_specific_one(void) {
    struct ehci_verdict_entry entries[2];
    struct ehci_verdict_choice choice;
    struct ehci_walk_report reset = report_root_reset();
    struct ehci_walk_report empty = report_empty();

    /* Both walks found nothing to bind; one failed at a reset, one had no
       device at all.  The machine says "reset", never "no device". */
    fill_entry(&entries[0], 0x1AU, EHCI_STATUS_OK, "READY", &reset, false);
    fill_entry(&entries[1], 0x1DU, EHCI_STATUS_OK, "READY", &empty, false);
    ehci_verdict_choose(entries, 2U, &choice);
    CHECK(choice.index == 0 && choice.status == EHCI_STATUS_PORT_NOT_ENABLED &&
              strcmp(choice.detail, "ROOT_RESET") == 0,
          "the reset refusal answers over the empty controller");
    CHECK(ehci_verdict_render(entries, 2U, NULL, 0U) == 0U,
          "a null buffer renders nothing");

    /* A controller that never started keeps its initialization refusal. */
    memset(entries, 0, sizeof(entries));
    ehci_verdict_fill(&entries[0], 0U, 0x1AU, 0U, false,
                      EHCI_STATUS_RESET_TIMEOUT, "PERIODIC_STOP_TIMEOUT", 6U,
                      NULL, false);
    fill_entry(&entries[1], 0x1DU, EHCI_STATUS_OK, "READY", &empty, false);
    ehci_verdict_choose(entries, 2U, &choice);
    CHECK(choice.index == 0 &&
              choice.status == EHCI_STATUS_RESET_TIMEOUT &&
              strcmp(choice.detail, "PERIODIC_STOP_TIMEOUT") == 0,
          "an initialization refusal outranks an empty walk");

    /* The attach path names its own status (`STORAGE_REFUSED`), and the
       aggregation keeps it instead of flattening it to the walk's stage. */
    memset(entries, 0, sizeof(entries));
    {
        struct ehci_walk_report attach;

        memset(&attach, 0, sizeof(attach));
        attach.ports_connected = 1U;
        attach.failures = 1U;
        attach.port_count = 1U;
        attach.ports[0].port = 1U;
        attach.ports[0].outcome = EHCI_WALK_OUTCOME_ATTACH;
        attach.ports[0].stage = "ATTACH";
        attach.outcome = EHCI_WALK_OUTCOME_ATTACH;
        attach.failure = "ATTACH";
        fill_entry(&entries[0], 0x1AU, EHCI_STATUS_STORAGE_REFUSED,
                   "CONFIG_PARSE", &attach, false);
        fill_entry(&entries[1], 0x1DU, EHCI_STATUS_OK, "READY", &empty, false);
        ehci_verdict_choose(entries, 2U, &choice);
        CHECK(choice.index == 0 &&
                  choice.status == EHCI_STATUS_STORAGE_REFUSED &&
                  strcmp(choice.detail, "CONFIG_PARSE") == 0,
              "the attach path's own refusal survives the aggregation");
    }
}

static void test_no_controller_at_all(void) {
    struct ehci_verdict_choice choice;

    ehci_verdict_choose(NULL, 0U, &choice);
    CHECK(choice.index == -1 && choice.controllers == 0U,
          "no controller means no chosen controller");
    CHECK(choice.status == EHCI_STATUS_CONTROLLER_ABSENT,
          "a machine with no EHCI function says so");
    CHECK(ehci_verdict_render(NULL, 0U, NULL, 0U) == 0U,
          "nothing to render");
}

static void test_the_list_is_bounded_and_whole(void) {
    struct ehci_verdict_entry entries[2];
    struct ehci_walk_report devices = report_devices8(
        "result=XACTERR index=0/3 qtd=0x00000048 received=0 residual=8");
    char text[24];
    size_t length;

    fill_entry(&entries[0], 0x1AU, EHCI_STATUS_OK, "READY", &devices, false);
    fill_entry(&entries[1], 0x1DU, EHCI_STATUS_OK, "READY", &devices, false);
    length = ehci_verdict_render(entries, 2U, text, sizeof(text));
    CHECK(length + 1U <= sizeof(text),
          "a short buffer never overruns");
    CHECK(text[length] == '\0',
          "the rendered list is always terminated");
    CHECK(length == 0U || text[length - 1U] != ';',
          "a truncated list never ends with a separator");
}

int main(void) {
    test_refusal_preserved_with_medium_on_the_second();
    test_the_same_machine_in_the_other_order();
    test_nothing_bound_names_the_first_concrete_refusal();
    test_a_generic_answer_never_replaces_a_specific_one();
    test_no_controller_at_all();
    test_the_list_is_bounded_and_whole();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI two-controller verdict host test: ALL OK\n");
    return 0;
}
