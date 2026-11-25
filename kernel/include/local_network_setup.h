#ifndef HFOC_LOCAL_NETWORK_SETUP_H
#define HFOC_LOCAL_NETWORK_SETUP_H

#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-447: the address the operator sets from the machine's own menu.
 *
 * ITEM-446 made the address a declaration on the install medium: the installed
 * appliance comes up on it and stores it in the persistent network record.
 * Changing it after the install still needed a new medium.  This module is the
 * menu half of the answer: the person standing at the machine picks the
 * Network entry, answers four bounded questions (mode, address, gateway, DNS)
 * and the appliance commits the answer into the same record the boot reads.
 *
 * The module is pure and total, in the style of ITEM-419's menu: it consumes
 * one entered line at a time, renders the next prompt or the refusal into the
 * caller's buffer, and never writes to a device, prints to a console or owns
 * the record.  The console performs the commit, because it is the caller that
 * holds the persistent owner - exactly as it holds the administrator
 * enrollment owner for ITEM-401.
 *
 * Nothing is guessed: a mode that is neither `dhcp` nor `static`, an address
 * without its prefix, a prefix outside 1..32, a gateway outside the declared
 * subnet or a malformed DNS refuses the whole answer, and the caller commits
 * only what the one IPv4 owner accepted.
 */

#define LOCAL_NETWORK_SETUP_VERSION 1U
#define LOCAL_NETWORK_SETUP_FIELD_MAX 32U
#define LOCAL_NETWORK_SETUP_PROMPT_MAX 96U
#define LOCAL_NETWORK_SETUP_RECORD_MAX 192U

enum local_network_setup_step {
    LOCAL_NETWORK_SETUP_STEP_IDLE = 0,
    LOCAL_NETWORK_SETUP_STEP_MODE = 1,
    LOCAL_NETWORK_SETUP_STEP_ADDRESS = 2,
    LOCAL_NETWORK_SETUP_STEP_GATEWAY = 3,
    LOCAL_NETWORK_SETUP_STEP_DNS = 4,
};

enum local_network_setup_status {
    LOCAL_NETWORK_SETUP_OK = 0,
    LOCAL_NETWORK_SETUP_NULL = 1,
    LOCAL_NETWORK_SETUP_BAD_STATE = 2,
    LOCAL_NETWORK_SETUP_CANCELLED = 3,
    LOCAL_NETWORK_SETUP_BUFFER_TOO_SMALL = 4,
};

/* What the caller has to do about one entered line. */
enum local_network_setup_action {
    LOCAL_NETWORK_SETUP_ACTION_NONE = 0,
    /* `output` holds the next prompt to print. */
    LOCAL_NETWORK_SETUP_ACTION_PROMPT = 1,
    /* The answer asked for discovery; commit it. */
    LOCAL_NETWORK_SETUP_ACTION_COMMIT_DHCP = 2,
    /* The answer asked for a static address; `config_out` holds it. */
    LOCAL_NETWORK_SETUP_ACTION_COMMIT_STATIC = 3,
    /* The operator cancelled; commit nothing. */
    LOCAL_NETWORK_SETUP_ACTION_CANCEL = 4,
    /* The answer is refused; `output` holds the refusal record. */
    LOCAL_NETWORK_SETUP_ACTION_REFUSED = 5,
};

struct local_network_setup {
    uint32_t version;
    bool active;
    enum local_network_setup_step step;
    bool static_selected;
    char address[LOCAL_NETWORK_SETUP_FIELD_MAX];
    char gateway[LOCAL_NETWORK_SETUP_FIELD_MAX];
    char dns[LOCAL_NETWORK_SETUP_FIELD_MAX];
};

/* Start the questions on the mode, discarding any earlier answer. */
void local_network_setup_begin(struct local_network_setup *setup);
/* Abandon an answer without committing anything. */
void local_network_setup_abort(struct local_network_setup *setup);
bool local_network_setup_active(const struct local_network_setup *setup);
enum local_network_setup_step local_network_setup_step(
    const struct local_network_setup *setup);

/* The prompt for the current step, without its leading newline. */
enum local_network_setup_status local_network_setup_prompt(
    const struct local_network_setup *setup,
    char *output,
    size_t capacity,
    size_t *output_length);

/*
 * Feed one entered line - without its newline - and report what to do next.
 * An empty line cancels the answer at every step.  `config_out` is written
 * only for COMMIT_STATIC; `output` is written for PROMPT and REFUSED.
 */
enum local_network_setup_status local_network_setup_feed(
    struct local_network_setup *setup,
    const char *line,
    size_t line_length,
    enum local_network_setup_action *action_out,
    char *output,
    size_t capacity,
    size_t *output_length,
    struct network_ipv4_config *config_out);

/*
 * ITEM-496: the record the caller publishes when the questions open, naming
 * what the interface has right now.  `current` is the caller's own rendering
 * (`DHCP`, `STATIC address=a.b.c.d/prefix`, ...); a missing one is published
 * as UNAVAILABLE rather than guessed.
 */
enum local_network_setup_status local_network_setup_offer(
    const char *current,
    size_t current_length,
    char *output,
    size_t capacity,
    size_t *output_length);

/*
 * The record the caller publishes after a commit or a cancellation.  The
 * address fields are only written for a static commit.  ITEM-496: `current`
 * is the same rendering `local_network_setup_offer()` takes, so the record
 * separates what was just stored from what the interface still runs - the
 * change reaches the wire at the next boot, and the record says which
 * configuration is on it until then.
 */
enum local_network_setup_status local_network_setup_record(
    bool is_static,
    const struct network_ipv4_config *config,
    bool persisted,
    const char *current,
    size_t current_length,
    char *output,
    size_t capacity,
    size_t *output_length);
/* The record the caller publishes when the answer was refused. */
enum local_network_setup_status local_network_setup_refusal(
    const char *reason,
    char *output,
    size_t capacity,
    size_t *output_length);
/* The record the caller publishes when no record owner is bound. */
enum local_network_setup_status local_network_setup_unavailable(
    char *output,
    size_t capacity,
    size_t *output_length);
const char *local_network_setup_status_string(
    enum local_network_setup_status status);

#endif
