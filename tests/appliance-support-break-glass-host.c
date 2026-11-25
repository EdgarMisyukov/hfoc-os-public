/*
 * ITEM-378 focused proof: documented break-glass recovery without a shell.
 *
 * The fixture proves the fixed action set, local-presence-only arming, the
 * two-phase single-use token, the bounded window, the refusal of every
 * remote attempt and the audit trail of every transition.
 */
#include "appliance_support_break_glass.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_SUPPORT_BREAK_GLASS_HOST_TEST

#define TEST_RENDER_CAPACITY 256U
#define TEST_TICK_BASE 5000U

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

static void test_declared_actions(void)
{
    CHECK(!appliance_support_break_glass_shell_required(),
          "break-glass never requires a shell");
    CHECK(appliance_support_break_glass_shell_action() == 0U,
          "the declared shell action is empty");
    CHECK(appliance_support_break_glass_declared_actions() ==
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
          "the declared action set is the fixed recovery list");
    CHECK(strcmp(appliance_support_break_glass_action_string(
                     APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_RESTORE_MANAGEMENT_LOCAL_ONLY),
                 "RESTORE_MANAGEMENT_LOCAL_ONLY") == 0 &&
              strcmp(appliance_support_break_glass_action_string(
                         APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_REVOKE_REMOTE_SUPPORT),
                     "REVOKE_REMOTE_SUPPORT") == 0 &&
              strcmp(appliance_support_break_glass_action_string(
                         0x80000000U),
                     "UNKNOWN") == 0,
          "action strings are the declared vocabulary");
    CHECK(strcmp(appliance_support_break_glass_schema_id(),
                 APPLIANCE_SUPPORT_BREAK_GLASS_SCHEMA_ID) == 0,
          "the break-glass schema id is the declared id");
}

static void test_local_presence_only(void)
{
    struct appliance_support_break_glass gate;
    struct appliance_support_break_glass_audit_entry entry;
    uint64_t token = 0U;

    CHECK(appliance_support_break_glass_init(&gate) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK,
          "break-glass initializes");
    CHECK(appliance_support_break_glass_state(&gate) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_STATE_STANDBY,
          "break-glass starts on standby");
    CHECK(!appliance_support_break_glass_armed(&gate, TEST_TICK_BASE),
          "break-glass starts unarmed");

    CHECK(appliance_support_break_glass_arm(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NETWORK, 1U,
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
              TEST_TICK_BASE, &token) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED,
          "arming over the network is refused");
    CHECK(appliance_support_break_glass_state(&gate) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_STATE_DENIED &&
              token == 0U,
          "a refused remote arm produces no token");
    CHECK(appliance_support_break_glass_audit_count(&gate) == 1U,
          "the refused remote arm is audited");
    memset(&entry, 0, sizeof(entry));
    CHECK(appliance_support_break_glass_audit_entry(&gate, 0U,
                                                    &entry) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK &&
              entry.event == APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_DENY &&
              entry.outcome ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED,
          "the denial audit entry records the remote refusal");

    CHECK(appliance_support_break_glass_arm(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              0U, APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
              TEST_TICK_BASE, &token) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_DENIED,
          "arming without an identified principal is refused");
    CHECK(appliance_support_break_glass_arm(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              1U, 0U, TEST_TICK_BASE, &token) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_BAD_ACTIONS,
          "arming with no action is refused");
    CHECK(appliance_support_break_glass_arm(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              1U, 0x80000000U, TEST_TICK_BASE, &token) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_BAD_ACTIONS,
          "arming with an undeclared action is refused");
    CHECK(appliance_support_break_glass_arm(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              1U,
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED | 0x40U,
              TEST_TICK_BASE, &token) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_BAD_ACTIONS,
          "arming with a command-shaped action is refused");
    CHECK(!appliance_support_break_glass_armed(&gate,
                                               TEST_TICK_BASE),
          "a refused action set never arms");
}

static void test_two_phase_token(void)
{
    struct appliance_support_break_glass gate;
    struct appliance_support_break_glass_audit_entry entry;
    uint64_t token = 0U;
    uint64_t second_token = 0U;
    uint32_t actions = 0U;

    (void)appliance_support_break_glass_init(&gate);
    CHECK(appliance_support_break_glass_arm(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              4U, APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
              TEST_TICK_BASE, &token) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK,
          "a local arm with the declared actions succeeds");
    CHECK(token != 0U &&
              appliance_support_break_glass_state(&gate) ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED,
          "arming publishes a token and the armed state");
    CHECK(appliance_support_break_glass_armed(
              &gate,
              TEST_TICK_BASE +
                  APPLIANCE_SUPPORT_BREAK_GLASS_CONFIRM_TICKS - 1U),
          "the window is open until the declared bound");
    CHECK(!appliance_support_break_glass_armed(
              &gate,
              TEST_TICK_BASE +
                  APPLIANCE_SUPPORT_BREAK_GLASS_CONFIRM_TICKS),
          "the window is closed at the declared bound");

    CHECK(appliance_support_break_glass_arm(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              4U,
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_REVOKE_REMOTE_SUPPORT,
              TEST_TICK_BASE + 1U, &second_token) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_ALREADY_ARMED,
          "a second arm while armed is refused");

    CHECK(appliance_support_break_glass_apply(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NETWORK,
              token, TEST_TICK_BASE + 2U, &actions) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED,
          "applying over the network is refused");
    CHECK(appliance_support_break_glass_state(&gate) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED &&
              actions == 0U,
          "a refused remote apply performs nothing");

    CHECK(appliance_support_break_glass_apply(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              token ^ 0xFFFFFFFFU, TEST_TICK_BASE + 3U, &actions) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_BAD_TOKEN,
          "a wrong token is refused");
    CHECK(appliance_support_break_glass_state(&gate) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED &&
              gate.deny_count == 2U,
          "a wrong token leaves the arm pending and is counted");

    CHECK(appliance_support_break_glass_apply(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              token, TEST_TICK_BASE + 4U, &actions) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK,
          "the exact token inside the window applies");
    CHECK(actions ==
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED &&
              appliance_support_break_glass_state(&gate) ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_STATE_APPLIED,
          "the applied action mask is published");
    CHECK(appliance_support_break_glass_applied_actions(&gate) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED &&
              gate.apply_count == 1U,
          "the applied mask is retained exactly once");
    CHECK(appliance_support_break_glass_apply(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              token, TEST_TICK_BASE + 5U, &actions) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM,
          "the token is single-use");

    memset(&entry, 0, sizeof(entry));
    CHECK(appliance_support_break_glass_audit_entry(&gate, 0U,
                                                    &entry) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK &&
              entry.event ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_ARM &&
              entry.token == token && entry.principal_id == 4U,
          "the arm audit entry names the token and principal");
    memset(&entry, 0, sizeof(entry));
    CHECK(appliance_support_break_glass_audit_entry(&gate, 3U,
                                                    &entry) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK &&
              entry.event ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_APPLY &&
              entry.actions ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
          "the apply audit entry names the applied actions");
}

static void test_window_expiry_and_cancel(void)
{
    struct appliance_support_break_glass gate;
    uint64_t token = 0U;
    uint32_t actions = 0U;

    (void)appliance_support_break_glass_init(&gate);
    (void)appliance_support_break_glass_arm(
        &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE, 4U,
        APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_REVOKE_REMOTE_SUPPORT,
        TEST_TICK_BASE, &token);
    CHECK(appliance_support_break_glass_refresh(
              &gate,
              TEST_TICK_BASE +
                  APPLIANCE_SUPPORT_BREAK_GLASS_CONFIRM_TICKS) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED,
          "the arm expires at the declared bound");
    CHECK(appliance_support_break_glass_state(&gate) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_STATE_EXPIRED &&
              gate.expire_count == 1U,
          "an expired arm publishes EXPIRED once");
    CHECK(appliance_support_break_glass_apply(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              token,
              TEST_TICK_BASE +
                  APPLIANCE_SUPPORT_BREAK_GLASS_CONFIRM_TICKS,
              &actions) == APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM,
          "an expired token cannot be applied");

    (void)appliance_support_break_glass_init(&gate);
    (void)appliance_support_break_glass_arm(
        &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE, 4U,
        APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
        TEST_TICK_BASE, &token);
    CHECK(appliance_support_break_glass_cancel(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NETWORK,
              TEST_TICK_BASE + 1U) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED,
          "cancelling over the network is refused");
    CHECK(appliance_support_break_glass_cancel(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              TEST_TICK_BASE + 2U) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK &&
              appliance_support_break_glass_state(&gate) ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_STATE_STANDBY,
          "a local cancel returns to standby");
    CHECK(appliance_support_break_glass_apply(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              token, TEST_TICK_BASE + 3U, &actions) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM,
          "a cancelled token cannot be applied");
}

static void test_unavailability_and_render(void)
{
    struct appliance_support_break_glass gate;
    char output[TEST_RENDER_CAPACITY];
    size_t output_length = 0U;
    uint64_t token = 0U;

    (void)appliance_support_break_glass_init(&gate);
    CHECK(appliance_support_break_glass_set_available(&gate, false) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK &&
              appliance_support_break_glass_state(&gate) ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNAVAILABLE,
          "an unavailable gate publishes UNAVAILABLE");
    CHECK(appliance_support_break_glass_arm(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
              4U, APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
              TEST_TICK_BASE, &token) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_UNAVAILABLE,
          "an unavailable gate refuses to arm");
    CHECK(appliance_support_break_glass_set_available(&gate, true) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_OK &&
              appliance_support_break_glass_state(&gate) ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_STATE_STANDBY,
          "a restored gate returns to standby");

    memset(output, 0, sizeof(output));
    CHECK(appliance_support_break_glass_render(
              &gate, TEST_TICK_BASE, output, sizeof(output),
              &output_length) == APPLIANCE_SUPPORT_BREAK_GLASS_OK,
          "render accepts a standby gate");
    CHECK(strstr(output, "break_glass v=1 state=STANDBY") != NULL &&
              strstr(output, "armed=NO") != NULL &&
              strstr(output, "shell=FORBIDDEN") != NULL &&
              strstr(output, "actions=NONE") != NULL,
          "render publishes the standby state without a shell");

    (void)appliance_support_break_glass_arm(
        &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE, 4U,
        APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
        TEST_TICK_BASE, &token);
    memset(output, 0, sizeof(output));
    CHECK(appliance_support_break_glass_render(
              &gate, TEST_TICK_BASE, output, sizeof(output),
              &output_length) == APPLIANCE_SUPPORT_BREAK_GLASS_OK &&
              strstr(output, "state=ARMED") != NULL &&
              strstr(output, "armed=YES") != NULL &&
              strstr(output,
                     "actions=RESTORE_MANAGEMENT_LOCAL_ONLY|"
                     "REVOKE_REMOTE_SUPPORT") != NULL &&
              strstr(output, "remaining=300") != NULL,
          "render publishes the armed window and declared actions");
    CHECK(output_length == strlen(output),
          "render reports the exact length");
    CHECK(appliance_support_break_glass_render(
              &gate, TEST_TICK_BASE, output, 8U, &output_length) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_BUFFER_TOO_SMALL,
          "render refuses a short buffer");
    CHECK(appliance_support_break_glass_render(
              NULL, TEST_TICK_BASE, output, sizeof(output),
              &output_length) == APPLIANCE_SUPPORT_BREAK_GLASS_NULL,
          "render refuses a null gate");

    CHECK(strcmp(appliance_support_break_glass_state_string(
                     APPLIANCE_SUPPORT_BREAK_GLASS_STATE_APPLIED),
                 "APPLIED") == 0 &&
              strcmp(appliance_support_break_glass_status_string(
                         APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED),
                     "REMOTE_REFUSED") == 0 &&
              strcmp(appliance_support_break_glass_event_string(
                         APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_APPLY),
                     "APPLY") == 0,
          "state, status and event strings are declared");
    CHECK(appliance_support_break_glass_state(NULL) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_STATE_NONE &&
              appliance_support_break_glass_last_status(NULL) ==
                  APPLIANCE_SUPPORT_BREAK_GLASS_NULL &&
              appliance_support_break_glass_audit_count(NULL) == 0U &&
              !appliance_support_break_glass_armed(NULL, 0U),
          "accessors fail closed on a null gate");
}

static void test_audit_ring_is_bounded(void)
{
    struct appliance_support_break_glass gate;
    uint64_t token = 0U;
    uint32_t index;

    (void)appliance_support_break_glass_init(&gate);
    for (index = 0U; index < 6U; ++index) {
        (void)appliance_support_break_glass_arm(
            &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
            4U, APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED,
            TEST_TICK_BASE + index, &token);
        (void)appliance_support_break_glass_cancel(
            &gate, APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE,
            TEST_TICK_BASE + index);
    }
    CHECK(appliance_support_break_glass_audit_count(&gate) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_AUDIT_MAX,
          "the audit trail is bounded by the declared capacity");
    CHECK(gate.audit_dropped == 4U,
          "dropped audit entries are counted instead of hidden");
    CHECK(appliance_support_break_glass_audit_entry(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_AUDIT_MAX, NULL) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_NULL,
          "a null audit output is refused");
    CHECK(appliance_support_break_glass_audit_entry(
              &gate, APPLIANCE_SUPPORT_BREAK_GLASS_AUDIT_MAX,
              &(struct appliance_support_break_glass_audit_entry){ 0 }) ==
              APPLIANCE_SUPPORT_BREAK_GLASS_BAD_STATE,
          "an out-of-range audit index is refused");
}

int main(void)
{
    test_declared_actions();
    test_local_presence_only();
    test_two_phase_token();
    test_window_expiry_and_cancel();
    test_unavailability_and_render();
    test_audit_ring_is_bounded();

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "the break-glass path performs no allocation");

    if (failures != 0) {
        fprintf(stderr,
                "Appliance support break-glass host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Appliance support break-glass host tests: ALL OK\n");
    return 0;
}

#endif
