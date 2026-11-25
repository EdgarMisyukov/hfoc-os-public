#ifndef HFOC_CONSOLE_H
#define HFOC_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_config_validation.h"
#include "appliance_host_seed.h"
#include "appliance_language.h"
#include "appliance_production_admin_enrollment.h"
#include "appliance_production_entropy.h"
#include "appliance_storage_report.h"
#include "keyboard.h"
#include "local_input.h"

/* ITEM-448: the console consumes the driver's decoded mouse events; the
   definition lives in usb_hid.h, and the console only forwards the pointer. */
struct usb_hid_mouse_event;

/* ITEM-466: the console publishes the [FB] record from the mode choice the
   kernel built; the definition lives in framebuffer.h. */
struct framebuffer_mode_choice;

void console_init(uint64_t usable_mib, uint32_t cpu_cores);
/*
 * ITEM-392: publish whether the local keyboard input path exists before the
 * console paints its readiness banner, so the banner never claims a healthy
 * keyboard the appliance does not have.
 */
void console_publish_local_input(bool available);
/*
 * ITEM-395: publish which keyboard path feeds the local console before the
 * banner is painted, so a machine driven by the USB HID keyboard does not
 * advertise a PS/2 keyboard it does not have (and vice versa).
 */
void console_publish_input_source(enum local_input_source source);
void console_publish_boot_persistence(
    bool storage_ready,
    uint32_t backend_count,
    enum appliance_config_validation_source config_source,
    const char *hostname,
    size_t hostname_length);
/*
 * ITEM-518: arm the console `storage` command with what the probe saw, before
 * the console loop can be reached and whether or not a state volume exists.
 * The boot publishes the probe's own report, the installer flow publishes the
 * same report with the discovery's verdict folded in, and the command renders
 * the last publication.  A machine that never probed answers `state=NOT_PROBED`
 * instead of a row of zeroes that would read like a finding.
 */
void console_publish_storage_report(const struct appliance_storage_report *report);
void console_write(const char *text);
/*
 * ITEM-394: publish the management address and URL on the local console.
 *
 * The management address used to reach only the serial record
 * ([PRODUCTION-DHCP] ... address=...), so a machine without a serial capture
 * could not learn where its panel is.  The console paints one line -
 *
 *   MANAGEMENT: https://<address>/ reachability=<scope> cert=<state>
 *
 * - once the network has an address, and leaves it on screen.  The scope is
 * the published management binding (LOCAL_ONLY means the panel is reachable
 * from the appliance itself only), and the certificate state names the
 * self-signed certificate the listener serves, so the browser warning an
 * operator will see is announced before it appears.
 */
void console_publish_management(
    const char *url,
    size_t url_length,
    const char *reachability,
    const char *certificate);
/*
 * ITEM-430: offer the machine's own menu when nobody asks for it.
 *
 * The console paints the rendered menu once per boot - after the idle window,
 * or at once on a machine with no local input at all - and publishes
 * `[LOCAL-MENU] offered reason=IDLE|NO_LOCAL_INPUT entries=6 keys=s,n,d,r,p,y`
 * (ITEM-455: the keys come from the menu's own table through
 * `local_menu_key_list()`, so the record cannot claim a key the menu does not
 * answer to).  The operator opens it with ENTER on the empty line; any other
 * key cancels the offer, so typing is never hijacked.  The idle offer line
 * also names the key that opens the network editor
 * (`PRESS ENTER FOR THE LOCAL MENU (N: NETWORK SETUP - SET THE ADDRESS
 * THERE)`), because that is where the static address is set and the owner
 * looked for the gesture without finding it.  The caller passes the loop's
 * millisecond clock, which the decision in local_menu.c owns the window for.
 */
void console_offer_local_menu(uint32_t now_ms);
/*
 * ITEM-405: mirror one bounded diagnostic record on the local console.
 *
 * The records this kernel publishes about an install, a refusal or the network
 * path that failed used to reach only the serial stream.  A machine without a
 * serial capture therefore showed nothing at all: the acceptance host installed
 * onto a disk whose ESP region still carried a foreign filesystem, the boot
 * stage refused, and the only explanation was on a port nobody was reading.
 *
 * This call paints the same record on the framebuffer console.  It is additive
 * and never feeds the serial stream - the record's owner has already written it
 * to serial, and this path paints the framebuffer only, so a mirrored record
 * cannot duplicate or reorder a serial line.  One call paints at most one
 * bounded record:
 *
 *   * a record longer than CONSOLE_RECORD_MAX is painted up to the bound and
 *     closed with "...", so a caller cannot flood the console with one line;
 *   * a block that carries several lines paints its first line only;
 *   * a record carrying a control character is refused;
 *   * a record identical to one of the last CONSOLE_RECORD_DEDUP records
 *     accepted is not repainted, so a poll loop that republishes an unchanged
 *     state does not scroll the screen;
 *   * before console_init() has painted the banner, records are buffered and
 *     flushed immediately after it, so a record published during early boot
 *     survives the banner and appears exactly once per boot.
 */
#define CONSOLE_RECORD_MAX 320U
#define CONSOLE_RECORD_DEDUP 4U
void console_publish_record(const char *record, size_t length);
/*
 * ITEM-481 (round 19): the durable-journal bound, made visible to the proof.
 *
 * `console_record_worst_length()` is the worst-case length of the record that
 * was assembled last (the same maxima the assembly enforces); a value at or
 * below `CONSOLE_RECORD_MAX` is the by-construction guarantee that no field of
 * that record could have been cut.  `console_record_spill_count()` counts the
 * `-TAIL` continuations a long record was published as, so a host proof can
 * tell "the record fitted" from "the record was split across bounded parts".
 */
size_t console_record_worst_length(void);
size_t console_record_spill_count(void);
size_t console_record_bound_violations(void);
/*
 * ITEM-405: assemble one mirrored record fragment by fragment, for the records
 * this kernel publishes from several `serial_write*()` calls inside one serial
 * sequence.  The serial calls stay exactly where they are - their formatting is
 * never re-implemented - and the same fragments are handed to the mirror:
 *
 *   console_record_begin();
 *   serial_write("[KBD ] state=");
 *   console_record_text("[KBD ] state=");
 *   ...
 *   console_record_end();     // paints, after the serial sequence has ended
 *
 * Appending only buffers; `console_record_end()` publishes through
 * `console_publish_record()` and therefore paints the framebuffer only.  The
 * mirror stops at the first newline and at CONSOLE_RECORD_MAX, so a record is
 * one bounded line however many fragments it was built from.
 */
void console_record_begin(void);
void console_record_text(const char *text);
void console_record_u64(uint64_t value);
void console_record_hex64(uint64_t value);
void console_record_end(void);
/*
 * ITEM-477 (round 15): the journal-only counterpart of the mirror above, for
 * the records published on a timer rather than on an event.
 *
 * A periodic heartbeat exists to keep the journal readable while nothing
 * changes, and it must reach the serial stream and the durable journal.  It
 * must not reach the screen: the screen budget (ITEM-466) exists so routine
 * records cannot push the operator's own view off a text console, and a record
 * whose only news is "still running" spends that budget on a body move the
 * reader gains nothing from.  The opener picks the funnel, and the fragments
 * are assembled exactly as the painted mirror assembles them - the same
 * `console_record_text()`/`_u64()`/`_hex64()` calls append to the same bounded
 * line - while `console_journal_end()` publishes that line through
 * `console_publish_journal()`, so the durable journal keeps the record and the
 * screen keeps its rows.
 *
 *   console_journal_begin();
 *   serial_write("[LOOP-PHASES] turns=");
 *   console_record_text("[LOOP-PHASES] turns=");
 *   ...
 *   console_journal_end();    // journal only, after the serial sequence
 *
 * A one-time state change still uses `console_record_end()`: it is exactly the
 * line the operator is supposed to see, and the screen budget admits it.
 */
void console_journal_begin(void);
void console_journal_end(void);
/*
 * ITEM-461: one bounded record for the durable journal alone.
 *
 * This is the same path the console's own transcript uses, exposed for the
 * records that exist to make a cost visible.  It stores the record through the
 * boot-log binding and does not paint it: publishing a cost record on the
 * screen would pay the very cost it measures, and a record published while a
 * pointer geometry record is being written would move the rows that record
 * just named.  The record is sanitised exactly like a painted one (one bounded
 * printable ASCII line) and is silently dropped when no log is bound.
 */
void console_publish_journal(const char *record, size_t length);
/*
 * ITEM-461/464: publish the display geometry once the framebuffer is
 * initialized.
 *
 *   [FB] width=<px> height=<px> pitch=<bytes> cells=<cols>x<rows> bpp=<bits>
 *        modes=<count> chosen=<WxH> requested=<WxH> actual=<WxH> fallback=<0|1>
 *        scale=<n> cell_px=<WxH> preferred=<WxH> available=<WxH,...>
 *
 * The owner's 2026-09-20 report asked whether the chosen resolution was the
 * cause of the slow console; nothing published the mode, so it could only be
 * guessed.  This is published through the record funnel, so it is painted once
 * after the banner and stored in the journal.  ITEM-464 added the firmware's
 * mode count and the mode the selection rule prefers.  ITEM-466 (round 5)
 * replaced the hardcoded request with the firmware's own preference: the
 * `framebuffer_mode_choice` names the first (most preferred) 32-bpp mode, the
 * mode the rule would choose when the preferred one was refused, and the
 * available list.  `requested=` is kept as the legacy name of the preferred
 * mode, and `available=` is the bounded list `...`-marked when it does not fit.
 */
void console_publish_display(uint64_t width, uint64_t height, uint64_t pitch,
                             uint32_t bpp, uint64_t mode_count,
                             const struct framebuffer_mode_choice *choice);
/*
 * ITEM-461/463: the line-paint and pointer counters, for the loop heartbeat
 * that publishes them on the serial stream where the QEMU proof reads them.
 * The journal keeps the same numbers as their own `[CONSOLE] paint` record.
 * The costs are microseconds (ITEM-463): the 100 Hz tick cannot resolve a
 * paint, so a millisecond reading was always zero.
 */
uint64_t console_render_line_count(void);
uint64_t console_render_char_count(void);
uint64_t console_render_max_us(void);
uint64_t console_render_paint_us(void);
uint64_t console_render_scroll_us(void);
uint64_t console_render_scroll_max_us(void);
uint64_t console_render_pointer_moves(void);
uint64_t console_render_pointer_max_us(void);
/* ITEM-466: records the screen budget kept off the screen (journal keeps all). */
uint64_t console_render_screen_suppressed(void);
/*
 * ITEM-474 (round 13): the console's own paint+scroll cost in microseconds, and
 * the per-turn scroll batch.  The main loop opens the batch once, calls the
 * turn's output as usual, and flushes it at the end of the turn; the cost is
 * what the loop's phase record subtracts from its other phases.
 */
uint64_t console_cost_us(void);
void console_begin_turn_batching(void);
void console_flush_deferred(void);
/*
 * ITEM-482 (round 20): the worst wait between a USB keyboard report being
 * decoded and its character reaching the screen, in microseconds.  The input
 * window record publishes it as `kbd_echo_us_max=`; the transport counters say
 * the reports arrive, this says whether the operator feels them arrive.
 */
uint64_t console_kbd_echo_us_max_value(void);
/*
 * MOUSE-DIAG (owner task 2026-09-27): the keyboard echo's own split, beside the
 * latency it already published.  `..._fast_count` is the ITEM-482 one-cell
 * paint, `..._redraw_count`/`_chars`/`_us_total`/`_us_max` the whole-line
 * redraws (text moves and ITEM-483's moved-generation fallback), and
 * `..._generation_invalidations` the subset of redraws the fast path would have
 * taken had nothing else painted.  Cumulative since boot; the loop publishes
 * window deltas.
 */
uint64_t console_input_echo_fast_count(void);
uint64_t console_input_echo_redraw_count(void);
uint64_t console_input_echo_redraw_chars(void);
uint64_t console_input_echo_redraw_us_total(void);
uint64_t console_input_echo_redraw_us_max(void);
uint64_t console_input_echo_generation_invalidations(void);
/*
 * ITEM-482 (round 20): the write-combining drain the keyboard path paid, in
 * microseconds, and the one drain that closes an input batch.  Normal typing
 * takes the fast path and does not call `console_write()`, so its echo costs no
 * drain at all; the redraw path (arrows, history) still drains its own write,
 * and the pointer's draws are drained once by `console_end_input_batch()`
 * after the keyboard and the bounded mouse batches have run.
 */
uint64_t console_keyboard_sync_us(void);
void console_end_input_batch(void);
#ifdef HFOC_CONSOLE_HOST_TEST
/* ITEM-466: start each host case with a fresh one-second screen window. */
void console_reset_screen_budget(void);
/* ITEM-474: the rows the batched scroll has moved, for the batch host proof. */
uint64_t console_test_scroll_rows(void);
#endif
/*
 * ITEM-461/463: the main loop's own turn cost, owned by kernel.c and repeated
 * by the `kbd` command so a proof reads the number after the load it drove.
 * `us` is microseconds; `max_us` the worst turn of the boot.
 */
uint64_t kernel_loop_iter_us(void);
uint64_t kernel_loop_iter_max_us(void);
/*
 * ITEM-470: the literal main-loop turns since boot, incremented in the same
 * function that times the turn, so `loops=` can be compared with the
 * service-pass count and the cycle-counter average.
 */
uint64_t kernel_loop_iteration_count(void);
/*
 * ITEM-469: end the loop-cost averaging window at the once-a-second input
 * heartbeat, so `kernel_loop_iter_us()` is an honest one-second average
 * instead of "since the last ten-second [PRODUCTION-LOOP] record happened to
 * clear it".
 */
void kernel_loop_iter_window_reset(void);
/*
 * ITEM-454: compare one console line against one of the console's own command
 * names without regard to ASCII case, and compare a command name that carries
 * an argument as a prefix (the name ends in the space that separates it from
 * the argument, as the `PTR ` and `ARM ` inputs do).
 *
 * `help` prints the names in upper case and the parser used to accept only the
 * lower-case spelling, so the operator the page's sign-in form sends to the
 * console was told `UNKNOWN COMMAND`.  Only the name is folded: the caller's
 * pointer to the argument is untouched, so an argument keeps the case it was
 * typed with.  Both answer false for a missing string.  ITEM-506: the prefix
 * form also feeds the redaction of the two removed `admin-provision <secret>`
 * / `admin-login <secret>` spellings, so a stale manual cannot push a password
 * into the durable record.
 */
bool console_command_is(const char *line, const char *command);
bool console_command_prefix(const char *line, const char *prefix);
/*
 * ITEM-401/ITEM-506: bind the live administrator enrollment owner so the one
 * console account family (`credentials`, `login`, `credentials reset`) can
 * verify, provision, sign in and reset through the persisted credential.
 */
struct appliance_production_admin_enrollment;
void console_bind_admin_enrollment(
    struct appliance_production_admin_enrollment *enrollment);
void console_bind_admin_credential_rekey(
    bool (*rekey)(
        void *context,
        const char *current_password,
        size_t current_password_length,
        const char *new_password,
        size_t new_password_length));
/*
 * ITEM-447: bind the network record so the machine's own menu can change the
 * address.  The console stores no verifier, no device and no record module:
 * the binding is the owner and the one function that commits a choice into it,
 * exactly as the boot path does.  Without the binding the Network entry keeps
 * publishing the record it always published.
 *
 * ITEM-496: the same binding answers what the interface has right now, because
 * the operator deciding on a new address has to be told the old one.  The
 * console still owns no model: `current` renders the caller's own live state
 * into a bounded record fragment (`DHCP`, `STATIC address=a.b.c.d/prefix`, ...)
 * and a missing callback or a failed rendering is published as UNAVAILABLE,
 * never guessed.
 */
struct network_ipv4_config;
struct console_network_binding {
    void *context;
    bool (*commit)(
        void *context,
        bool is_static,
        const struct network_ipv4_config *config);
    bool (*current)(
        void *context,
        char *output,
        size_t capacity,
        size_t *output_length);
};
void console_bind_network_config(
    const struct console_network_binding *binding);
/*
 * ITEM-506: bind the one account to the console.
 *
 * The console asks the questions, reads the answers without echo and never
 * keeps a password: the binding is the enrollment owner that verifies,
 * provisions, authenticates and resets, exactly like the panel's own
 * credential boundary.  The conversation belongs to the console because the
 * prompt text and the hidden line belong to it; the policy, the KDF and the
 * record belong to the owner.  A missing binding answers UNAVAILABLE instead
 * of inventing an account.
 *
 * `status` publishes the owner's own bounded state record (the
 * `[ADMIN-ENROLLMENT] v=1 state=...` line the boot and the acceptance runbook
 * read) and may be NULL; `status_text` names a refusal and may be NULL, in
 * which case the console says UNKNOWN rather than guessing.
 */
struct console_admin_credentials_binding {
    void *context;
    enum appliance_production_admin_enrollment_status (*verify)(
        void *context,
        const char *password,
        size_t password_length,
        uint64_t now_tick);
    enum appliance_production_admin_enrollment_status (*provision)(
        void *context,
        const char *login,
        size_t login_length,
        const char *password,
        size_t password_length,
        uint64_t now_tick);
    /*
     * ITEM-598: before an existing account is replaced, re-seal the optional
     * host seed from the proven current password to the new password.  The
     * callback is a no-op when this appliance has no host seed; a false result
     * refuses the account change before the credential record is written.
     */
    bool (*rekey_credential)(
        void *context,
        const char *current_password,
        size_t current_password_length,
        const char *new_password,
        size_t new_password_length);
    enum appliance_production_admin_enrollment_status (*authenticate)(
        void *context,
        const char *login,
        size_t login_length,
        const char *password,
        size_t password_length,
        uint64_t now_tick,
        uint32_t lifetime_ticks,
        uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
        uint32_t *session_id_out);
    enum appliance_production_admin_enrollment_status (*reset)(
        void *context);
    void (*status)(void *context);
    const char *(*status_text)(
        enum appliance_production_admin_enrollment_status status);
};
void console_bind_admin_credentials(
    const struct console_admin_credentials_binding *binding);
/*
 * ITEM-598: the ordinary boot login that unlocks the host-provisioned seed.
 * It reuses the hidden login/password conversation, but the callback
 * authenticates the account and advances the durable entropy epoch instead of
 * issuing a browser bearer token.
 */
enum console_entropy_login_stage {
    CONSOLE_ENTROPY_LOGIN_STAGE_CONFIGURATION = 0,
    CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT = 1,
    CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT_PUBLISH = 2,
    CONSOLE_ENTROPY_LOGIN_STAGE_SEED = 3,
    CONSOLE_ENTROPY_LOGIN_STAGE_ENTROPY = 4,
    CONSOLE_ENTROPY_LOGIN_STAGE_COMPLETE = 5,
};
struct console_entropy_login_result {
    enum console_entropy_login_stage stage;
    enum appliance_production_admin_enrollment_status account_status;
    enum appliance_host_seed_status seed_status;
    enum appliance_production_entropy_seed_status entropy_status;
};
struct console_entropy_login_binding {
    void *context;
    struct console_entropy_login_result (*unlock)(
        void *context,
        const char *login,
        size_t login_length,
        const char *password,
        size_t password_length,
        uint64_t now_tick);
};
void console_bind_entropy_login(
    const struct console_entropy_login_binding *binding);
void console_start_entropy_unlock(void);
/*
 * ITEM-509 increment 4: bind the durable interface language to the console.
 *
 * The console owns the vocabulary (`language`, `language en|ru`) and the
 * answers; the owner owns the system record.  `write` commits a new language
 * and answers whether the record was written, so a machine without a usable
 * record refuses visibly instead of reporting a change that did not happen.
 * The console keeps the language it was published with
 * (`console_publish_language()`) and updates it only after `write` says yes.
 */
struct console_language_binding {
    void *context;
    bool (*write)(void *context, enum appliance_language language);
};
void console_bind_language(const struct console_language_binding *binding);
/*
 * ITEM-509 increment 4: the language the console and the panel are set to, and
 * whether this machine has a durable record to change.  Published by the boot
 * that loaded the system record; the console never guesses one.
 */
void console_publish_language(enum appliance_language language,
                              bool durable);
/*
 * ITEM-509 increment 4: the host proof asks which language the console is
 * answering in and whether it may write, so a switch that only printed a record
 * is observable.
 */
enum appliance_language console_active_language(void);
bool console_language_is_durable(void);
/*
 * ITEM-516: bind the durable management gate to the console.
 *
 * The console owns the vocabulary (`gate`, `gate open`, `gate open confirm`,
 * `gate close`) and the warning; the owner owns the system record.  `read`
 * answers the setting the record holds, `write` commits one and answers
 * whether it was written, so a machine without a usable record refuses
 * visibly instead of reporting a change that did not happen.
 *
 * Enabling is local-only: the console keeps its own flag of a proven local
 * secret (set by a successful `login` conversation, or by the current-password
 * step of `credentials`) and `gate open confirm` refuses without it.  The
 * panel can never reach this command, and there is no anonymous path to it.
 */
struct console_gate_binding {
    void *context;
    bool (*read)(void *context, bool *open_out);
    bool (*write)(void *context, bool open);
};
void console_bind_gate(const struct console_gate_binding *binding);
/*
 * The setting the boot published, and whether this machine has a durable
 * record to change.  A machine whose record did not load publishes the default
 * closed gate and no binding, so `gate open` answers that the setting is
 * unavailable instead of pretending it was saved.
 */
void console_publish_gate(bool open, bool durable);
bool console_gate_is_open(void);
bool console_gate_is_durable(void);
/*
 * The host proof asks whether a local secret was proven in this session, so a
 * refusal that only printed a record is observable.
 */
bool console_gate_local_secret_proven(void);
/*
 * ITEM-506: the host proof asks whether the hidden input mode is on, so a
 * regression that echoes a typed password into the transcript is observable
 * without reading the screen.
 */
bool console_admin_credentials_input_active(void);
/*
 * ITEM-410: bind the appliance boot log to the console.
 *
 * The console owns the record funnel (`console_publish_record()`), so it is the
 * one place where every published record can be handed to the boot log as it is
 * produced - including the records a machine without a serial capture shows on
 * screen for a few seconds and then scrolls away.  The binding carries the two
 * things the console needs and nothing else:
 *
 *   * `observe` sees each accepted record once, with the same bounded,
 *     printable line the console paints, so the log stores what the operator
 *     read;
 *   * `dump` answers the local `BOOTLOG` command from the log's own RAM ring -
 *     deliberately not from the disk, because the command that explains why a
 *     run refused must not depend on the write path that refused.
 *
 * The log never writes to the serial stream: it is a sink, not a source.
 */
struct console_boot_log_binding {
    void (*observe)(const char *record, size_t length);
    void (*dump)(void);
};
void console_bind_boot_log(const struct console_boot_log_binding *binding);
void console_putc(char value);
void console_process_input(char value);
void console_process_event(const struct keyboard_event *event);
/*
 * ITEM-448: one decoded mouse report.  The pointer starts in the middle of the
 * screen, moves by the report's signed deltas, and a left click runs the menu
 * entry whose painted line it landed on - the same entry the keyboard's ENTER
 * would run - publishing `[POINTER] click ...` so the action is evidence
 * rather than a screenshot.  A machine with no framebuffer ignores the event.
 */
void console_process_mouse_event(const struct usb_hid_mouse_event *event);
/*
 * ITEM-480 (round 18): one coalesced pointer batch.  A run of movements draws
 * the arrow once and drains the write-combining stores once; a button edge is a
 * batch of its own, applied in order.  This is the entry point the ordinary loop
 * uses, called at most `LOCAL_INPUT_MOUSE_DRAW_BUDGET` times per turn.
 */
void console_process_mouse_batch(const struct usb_hid_mouse_batch *batch);
/*
 * ITEM-484 (round 22): the pointer becomes visible when the machine learns a
 * mouse is bound, not when the first report moves it and not when the operator
 * clicks.  The loop calls this while `local_input_mouse_ready()` is true; the
 * first call centres the arrow and paints it exactly once, and every later call
 * is a no-op.  A zero-movement report leaves the visible arrow alone.
 */
void console_pointer_ready(void);

/* ITEM-448: where the arrow is, and whether a mouse has placed it yet. */
bool console_pointer_position(int32_t *x, int32_t *y);
/*
 * ITEM-472 (СРОЧНО-22): the once-a-second pointer record.  The main loop calls
 * it every turn; it publishes only when the PIT-calibrated wall clock has
 * crossed a second with new movements, so the cadence no longer depends on how
 * many IRQ ticks the CPU managed to take.
 */
void console_pointer_tick(void);
void console_update_uptime(uint64_t seconds);

/* ITEM-601: the local SSH enrollment conversation.  The identity owner is
   bound after the SSH owner prepares it; `ssh-key add` only stages the
   validated point and `ssh-key confirm` writes it, mirroring the ITEM-465/506
   two-line confirmation discipline. */
struct appliance_ssh_identity;
void console_bind_ssh_identity(struct appliance_ssh_identity *identity);
/* ITEM-601: a credential reset revokes the SSH client key and closes sessions. */
void console_bind_ssh_revoke(bool (*revoke)(void *context), void *context);

#ifdef HFOC_CONSOLE_HOST_TEST
void console_test_reset(void);
void console_test_add_history(const char *command);
const char *console_test_line(void);
size_t console_test_cursor(void);
/* ITEM-457: how many further lines the body holds before the next newline
   scrolls it, so the host proof can fill the screen to its last row without
   guessing the banner's length - and then require that a line that still fits
   scrolls nothing and publishes nothing. */
uint64_t console_test_rows_until_scroll(void);
/* ITEM-448: the menu rectangles the last paint recorded, so the host proof can
   aim a synthetic click at a real line. */
size_t console_test_menu_target_count(void);
bool console_test_menu_target(size_t index, int32_t *x, int32_t *y,
                              int32_t *width, int32_t *height);
/* ITEM-509 increment 24: the power refusal line and the already-open account
   conversation can only be reached through branches the host build compiles
   out or the keyboard captures; these hooks call the same functions the kernel
   calls, so the proof reads the exact published line. */
void console_test_power_refusal(const char *action);
void console_test_admin_credentials_reenter(void);
#endif

#endif
