#include "appliance_panel_gate.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/*
 * ITEM-516: the management gate's records are contract text.  The acceptance
 * report reads the boot line, the operator reads the console line, and the
 * panel's refusal name is what a script and a host proof compare against, so
 * every byte is pinned here rather than described.
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

static void test_boot_records(void)
{
    CHECK(strcmp(appliance_panel_gate_record(
                     APPLIANCE_PANEL_GATE_EVENT_BOOT_PUBLISHED),
                 "[PRODUCTION-SECURITY] gate=PUBLISHED source=POLICY") == 0,
          "the policy publication record is exact");
    CHECK(strcmp(appliance_panel_gate_record(
                     APPLIANCE_PANEL_GATE_EVENT_BOOT_CLOSED),
                 "[PRODUCTION-SECURITY] gate=CLOSED "
                 "reason=AWAITING_LOCAL_SECRET") == 0,
          "the awaiting-local-secret record is exact");
}

static void test_console_records(void)
{
    CHECK(strcmp(appliance_panel_gate_record(
                     APPLIANCE_PANEL_GATE_EVENT_PENDING),
                 "[PRODUCTION-SECURITY] gate=PENDING action=open "
                 "confirmation_required") == 0,
          "the pending confirmation record is exact");
    CHECK(strcmp(appliance_panel_gate_setting_record(true),
                 "[PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET") ==
              0,
          "the enabled record is exact and names the local secret");
    CHECK(strcmp(appliance_panel_gate_setting_record(false),
                 "[PRODUCTION-SECURITY] gate=DISABLED source=CONSOLE") == 0,
          "the disabled record is exact and names the console");
    CHECK(strcmp(appliance_panel_gate_record(
                     APPLIANCE_PANEL_GATE_EVENT_REFUSED),
                 "[PRODUCTION-SECURITY] gate=REFUSED "
                 "reason=GATE_LOCAL_SECRET_REQUIRED") == 0,
          "the local-secret refusal record is exact");
}

static void test_event_names(void)
{
    CHECK(strcmp(appliance_panel_gate_event_string(
                     APPLIANCE_PANEL_GATE_EVENT_BOOT_PUBLISHED),
                 "BOOT_PUBLISHED") == 0 &&
              strcmp(appliance_panel_gate_event_string(
                         APPLIANCE_PANEL_GATE_EVENT_BOOT_CLOSED),
                     "BOOT_CLOSED") == 0,
          "the boot events have stable names");
    CHECK(appliance_panel_gate_record(APPLIANCE_PANEL_GATE_EVENT_NONE) ==
              NULL &&
              appliance_panel_gate_record(
                  (enum appliance_panel_gate_event)99) == NULL &&
              appliance_panel_gate_event_string(
                  (enum appliance_panel_gate_event)99) == NULL,
          "an event the module does not name answers nothing");
    CHECK(strcmp(APPLIANCE_PANEL_GATE_LOCAL_SECRET_REQUIRED,
                 "GATE_LOCAL_SECRET_REQUIRED") == 0,
          "the panel refusal name is stable");
    CHECK(strcmp(APPLIANCE_PANEL_GATE_VALUE_OPEN, "open") == 0 &&
              strcmp(APPLIANCE_PANEL_GATE_VALUE_CLOSED, "closed") == 0,
          "the durable values are the record's own words");
}

int main(void)
{
    test_boot_records();
    test_console_records();
    test_event_names();
    if (failures != 0) {
        fprintf(stderr, "Appliance panel gate host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance panel gate host tests: ALL OK");
    return 0;
}
