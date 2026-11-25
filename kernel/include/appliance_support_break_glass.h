#ifndef HFOC_APPLIANCE_SUPPORT_BREAK_GLASS_H
#define HFOC_APPLIANCE_SUPPORT_BREAK_GLASS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-378 documented break-glass recovery, no shell required.
 *
 * Break-glass is the procedure a locally present administrator follows when
 * the management plane is unreachable or its administrative credential is
 * lost.  It is deliberately not a shell and not a command channel:
 *
 *   - the action set is a fixed, declared list: restore the management
 *     ingress policy to LOCAL_ONLY and revoke any remote support session.
 *     There is no free-form input, no path, no command id and no scripting
 *     runtime, and the module declares that no shell is required;
 *   - it is local-presence only: an arm or apply attempt that arrives over
 *     the network is refused outright, before any effect;
 *   - it is two-phase: arming produces a single-use token bounded by
 *     APPLIANCE_SUPPORT_BREAK_GLASS_CONFIRM_TICKS; applying requires the
 *     exact token inside the window exactly once, so a replay, a wrong token
 *     or a stale window all refuse;
 *   - every transition is audited, and the applied action mask is published
 *     so the composition can perform the effects and the operator can see
 *     exactly what was done.
 *
 * Allocation-free; the module owns only the bounded state below.  The
 * effects themselves stay with the composition that owns the management
 * ingress policy and the support session.
 */

#define APPLIANCE_SUPPORT_BREAK_GLASS_VERSION 1U
#define APPLIANCE_SUPPORT_BREAK_GLASS_SCHEMA_ID "HFOC-BREAK-GLASS-1"
#define APPLIANCE_SUPPORT_BREAK_GLASS_SCHEMA_ID_LENGTH 17U
#define APPLIANCE_SUPPORT_BREAK_GLASS_CONFIRM_TICKS 300U
#define APPLIANCE_SUPPORT_BREAK_GLASS_AUDIT_MAX 8U
#define APPLIANCE_SUPPORT_BREAK_GLASS_RENDER_MAX 200U

#define APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_NONE 0U
#define APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_RESTORE_MANAGEMENT_LOCAL_ONLY \
    (1U << 0)
#define APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_REVOKE_REMOTE_SUPPORT \
    (1U << 1)
#define APPLIANCE_SUPPORT_BREAK_GLASS_ACTIONS_DECLARED \
    (APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_RESTORE_MANAGEMENT_LOCAL_ONLY | \
     APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_REVOKE_REMOTE_SUPPORT)
/* Declared absence of a command or shell action. */
#define APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_SHELL 0U

enum appliance_support_break_glass_origin {
    APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NONE = 0,
    APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_LOCAL_CONSOLE = 1,
    APPLIANCE_SUPPORT_BREAK_GLASS_ORIGIN_NETWORK = 2,
};

enum appliance_support_break_glass_state {
    APPLIANCE_SUPPORT_BREAK_GLASS_STATE_NONE = 0,
    APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNINITIALIZED = 1,
    APPLIANCE_SUPPORT_BREAK_GLASS_STATE_UNAVAILABLE = 2,
    APPLIANCE_SUPPORT_BREAK_GLASS_STATE_STANDBY = 3,
    APPLIANCE_SUPPORT_BREAK_GLASS_STATE_ARMED = 4,
    APPLIANCE_SUPPORT_BREAK_GLASS_STATE_APPLIED = 5,
    APPLIANCE_SUPPORT_BREAK_GLASS_STATE_EXPIRED = 6,
    APPLIANCE_SUPPORT_BREAK_GLASS_STATE_DENIED = 7,
};

enum appliance_support_break_glass_status {
    APPLIANCE_SUPPORT_BREAK_GLASS_OK = 0,
    APPLIANCE_SUPPORT_BREAK_GLASS_NULL = 1,
    APPLIANCE_SUPPORT_BREAK_GLASS_BAD_STATE = 2,
    APPLIANCE_SUPPORT_BREAK_GLASS_BAD_VERSION = 3,
    APPLIANCE_SUPPORT_BREAK_GLASS_UNAVAILABLE = 4,
    APPLIANCE_SUPPORT_BREAK_GLASS_REMOTE_REFUSED = 5,
    APPLIANCE_SUPPORT_BREAK_GLASS_DENIED = 6,
    APPLIANCE_SUPPORT_BREAK_GLASS_NO_ARM = 7,
    APPLIANCE_SUPPORT_BREAK_GLASS_ALREADY_ARMED = 8,
    APPLIANCE_SUPPORT_BREAK_GLASS_BAD_TOKEN = 9,
    APPLIANCE_SUPPORT_BREAK_GLASS_EXPIRED = 10,
    APPLIANCE_SUPPORT_BREAK_GLASS_BAD_ACTIONS = 11,
    APPLIANCE_SUPPORT_BREAK_GLASS_BUFFER_TOO_SMALL = 12,
};

enum appliance_support_break_glass_event {
    APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_NONE = 0,
    APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_ARM = 1,
    APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_APPLY = 2,
    APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_CANCEL = 3,
    APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_DENY = 4,
    APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_EXPIRE = 5,
    APPLIANCE_SUPPORT_BREAK_GLASS_EVENT_UNAVAILABLE = 6,
};

struct appliance_support_break_glass_audit_entry {
    uint64_t tick;
    uint64_t token;
    uint32_t event;
    uint32_t outcome;
    uint32_t principal_id;
    uint32_t actions;
    uint32_t origin;
};

struct appliance_support_break_glass {
    bool initialized;
    uint32_t version;
    enum appliance_support_break_glass_state state;
    enum appliance_support_break_glass_status last_status;
    bool available;
    uint64_t token_sequence;
    uint64_t pending_token;
    uint64_t arm_tick;
    uint64_t expiry_tick;
    uint32_t pending_actions;
    uint32_t principal_id;
    uint64_t applied_tick;
    uint32_t applied_actions;
    uint32_t applied_principal;
    uint32_t arm_count;
    uint32_t apply_count;
    uint32_t cancel_count;
    uint32_t deny_count;
    uint32_t expire_count;
    uint32_t unavailable_count;
    struct appliance_support_break_glass_audit_entry
        audit[APPLIANCE_SUPPORT_BREAK_GLASS_AUDIT_MAX];
    uint32_t audit_count;
    uint32_t audit_dropped;
};

enum appliance_support_break_glass_status
appliance_support_break_glass_init(
    struct appliance_support_break_glass *gate);
enum appliance_support_break_glass_status
appliance_support_break_glass_set_available(
    struct appliance_support_break_glass *gate,
    bool available);
enum appliance_support_break_glass_status
appliance_support_break_glass_arm(
    struct appliance_support_break_glass *gate,
    enum appliance_support_break_glass_origin origin,
    uint32_t principal_id,
    uint32_t actions,
    uint64_t now_tick,
    uint64_t *token_out);
enum appliance_support_break_glass_status
appliance_support_break_glass_apply(
    struct appliance_support_break_glass *gate,
    enum appliance_support_break_glass_origin origin,
    uint64_t token,
    uint64_t now_tick,
    uint32_t *actions_out);
enum appliance_support_break_glass_status
appliance_support_break_glass_cancel(
    struct appliance_support_break_glass *gate,
    enum appliance_support_break_glass_origin origin,
    uint64_t now_tick);
enum appliance_support_break_glass_status
appliance_support_break_glass_refresh(
    struct appliance_support_break_glass *gate,
    uint64_t now_tick);
bool appliance_support_break_glass_armed(
    const struct appliance_support_break_glass *gate,
    uint64_t now_tick);
bool appliance_support_break_glass_shell_required(void);
uint32_t appliance_support_break_glass_shell_action(void);
uint32_t appliance_support_break_glass_declared_actions(void);
uint32_t appliance_support_break_glass_applied_actions(
    const struct appliance_support_break_glass *gate);
enum appliance_support_break_glass_state
appliance_support_break_glass_state(
    const struct appliance_support_break_glass *gate);
enum appliance_support_break_glass_status
appliance_support_break_glass_last_status(
    const struct appliance_support_break_glass *gate);
uint32_t appliance_support_break_glass_audit_count(
    const struct appliance_support_break_glass *gate);
enum appliance_support_break_glass_status
appliance_support_break_glass_audit_entry(
    const struct appliance_support_break_glass *gate,
    uint32_t index,
    struct appliance_support_break_glass_audit_entry *entry_out);
enum appliance_support_break_glass_status
appliance_support_break_glass_render(
    const struct appliance_support_break_glass *gate,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_support_break_glass_state_string(
    enum appliance_support_break_glass_state state);
const char *appliance_support_break_glass_status_string(
    enum appliance_support_break_glass_status status);
const char *appliance_support_break_glass_event_string(
    enum appliance_support_break_glass_event event);
const char *appliance_support_break_glass_action_string(uint32_t action);
const char *appliance_support_break_glass_schema_id(void);

#endif
