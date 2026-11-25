#include "console.h"
#include "appliance_admin_credentials_prompt.h"
#include "appliance_language.h"
#include "console_credentials_command.h"
#include "console_gate_command.h"
#include "console_help_command.h"
#include "console_language_command.h"
#include "appliance_console.h"
#include "appliance_boot_console.h"
#include "appliance_identity.h"
#include "appliance_panel_gate.h"
#include "appliance_production_admin_enrollment.h"
#ifdef HFOC_PRODUCTION_TLS
#include "appliance_production_entropy.h"
#include "appliance_ssh_identity.h"
#endif
#include "arch/x86_64/io.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/timer.h"
#include "framebuffer.h"
#include "heap.h"
#include "hfoc_lwip_time.h"
#include "kstring.h"
#include "keyboard.h"
#include "local_input.h"
#include "local_menu.h"
#include "kernel_status_report.h"
#include "local_network_setup.h"
#include "pointer.h"
#ifndef HFOC_CONSOLE_HOST_TEST
#include "appliance_boot_log_store.h"
#include "platform_power.h"
#include "platform_reset.h"
#endif
#include "diag_export.h"
#include "install_journal_platform.h"
#include "hfoc_build.h"
#include "record_line.h"
#include "ehci.h"
#include "xhci.h"
#include "panic.h"
#include "platform_power.h"
#include "pmm.h"
#ifdef HFOC_PRODUCTION_INSTALL_MEDIA
#include "production_install_flow.h"
#endif
#include "serial.h"
#include "vmm.h"
#include <stddef.h>
#include <stdint.h>

#define BACKGROUND 0x000000
#define HEADER 0x000000
#define TEXT 0xf5f5f5
#define ACCENT 0x86efac
#define MUTED 0xb8b8b8
/*
 * ITEM-466: the glyph scale is a property of the mode, not a constant.  A text
 * console shows the same characters at every mode, so the mode only decides
 * how large the cell is; `console_scale` is computed from the actual mode
 * (framebuffer_font_scale()) and the two cell sizes follow it.  The defaults
 * match the old fixed scale so a host case that never initialises a
 * framebuffer still has a defined geometry.
 */
static uint32_t console_scale = 2U;
static uint64_t console_cell_width = FRAMEBUFFER_FONT_CELL_WIDTH * 2U;
static uint64_t console_cell_height = FRAMEBUFFER_FONT_CELL_HEIGHT * 2U;
#define LEFT 12U
#define BODY_TOP 78U
/*
 * ITEM-506: the account policy allows a 128-character password, and a hidden
 * line must be able to hold the whole maximum: the cap is the policy's maximum,
 * not one less, so a password is refused by the policy rather than cut by the
 * line buffer.  A 129th character is ignored, exactly as the old cap ignored
 * the 128th; the conversation's own store checks the length against the model
 * and refuses an over-long line, so the cap never silently truncates what the
 * operator typed.
 */
#define INPUT_MAX 128U
#define PAGE_FAULT_ADDRESS UINT64_C(0x0000400000000000)
#define HISTORY_CAPACITY 16U

static bool boot_persistence_published;
static bool boot_storage_ready;
static uint32_t boot_backend_count;
static enum appliance_config_validation_source boot_config_source;
static char boot_hostname[
    APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX + 1U];
/*
 * ITEM-392: the local console is only as healthy as its input path, so it
 * never claims a keyboard before the boot has published one. The default is
 * "not proven available" rather than "assumed available".
 */
static bool local_input_available;
/* ITEM-395: the path that actually feeds this console: PS2, USB_HID or NONE. */
static enum local_input_source input_source;
/*
 * ITEM-394: the management line is painted when the network first has an
 * address, and repainted when the address changes, so the console never keeps
 * a stale URL on screen.
 */
static char management_url[APPLIANCE_BOOT_CONSOLE_URL_MAX + 1U];
static bool management_published;
/*
 * ITEM-648: the management address is a record, not a credential answer.  A
 * login/password question owns the current line, so a publication that lands
 * inside that conversation is held as the newest bounded notice and painted
 * once, on its own line, at the next prompt boundary.  The URL, reachability
 * and certificate are copied rather than borrowed because the caller's text
 * need not outlive the call.
 */
#define CONSOLE_MANAGEMENT_STATE_MAX 64U
static char management_pending_url[APPLIANCE_BOOT_CONSOLE_URL_MAX + 1U];
static char management_pending_reachability[CONSOLE_MANAGEMENT_STATE_MAX + 1U];
static char management_pending_certificate[CONSOLE_MANAGEMENT_STATE_MAX + 1U];
static bool management_pending;

/* ITEM-461: the mode the boot published, kept so the `kbd` command can repeat
   it on the serial stream the QEMU proof reads. */
static uint64_t display_width;
static uint64_t display_height;
static uint64_t display_pitch;
static uint32_t display_bpp;
/* ITEM-463: the requested mode and whether the actual one is a fallback. */
static uint64_t display_requested_width;
static uint64_t display_requested_height;
static uint32_t display_fallback;
/* ITEM-466: how many 32-bpp modes the firmware offered. */
static uint64_t display_available_count;

/*
 * ITEM-405: the diagnostic records mirrored from the serial stream.
 *
 * `record_line_max` is one bounded painted line: the accepted prefix of a
 * record plus the "..." that marks a record cut at the bound.  Records accepted
 * before console_init() painted the banner wait in `record_pending` (a FIFO
 * that drops its oldest entry rather than its newest, because the newest record
 * is the one that explains an outcome), and every accepted record is remembered
 * in `record_dedup` so a poll loop republishing an unchanged state does not
 * scroll the screen.
 */
#define CONSOLE_RECORD_LINE_MAX (CONSOLE_RECORD_MAX + 3U)
#define CONSOLE_RECORD_PENDING 16U
/*
 * ITEM-481 (round 19): the durable-journal record funnel bounds every builder
 * by construction instead of cutting the tail off a long record.
 *
 * The console mirror used to assemble one line and let `console_record_accept()`
 * cut anything past CONSOLE_RECORD_MAX with "...".  A record whose worst case
 * crosses the bound - a counter that grows, or one more field - therefore lost
 * its tail silently, and the 2026-09-25 hardware journal shows the
 * `[PRODUCTION-LOOP]` record ending at `malformed...` exactly that way.
 *
 * The assembled record is now held whole in a buffer sized to the worst case of
 * the longest builder (`[PRODUCTION-LOOP]`: fifty-four counters at twenty digits
 * plus its literals), and the bound is enforced while it is assembled: a
 * fragment that would cross CONSOLE_RECORD_MAX records a break at that point, so
 * each part's worst case is at or below the bound.  `console_mirror_finish()`
 * turns the breaks into a named head and short `[<name>-TAIL]` continuations
 * after the serial sequence has ended: publishing from inside it would let the
 * log's own `[BOOTLOG]` serial record split the line the sequence is still
 * writing.  `console_publish_journal()` applies the same bound to a record a
 * caller built itself.
 */
#define CONSOLE_RECORD_U64_WORST 20U
#define CONSOLE_RECORD_HEX64_WORST 18U
#define CONSOLE_RECORD_ASSEMBLY_MAX 2560U
#define CONSOLE_RECORD_NAME_MAX 48U
/* The worst case of one `[<name>-TAIL]` prefix: brackets, name, suffix. */
#define CONSOLE_RECORD_TAIL_WORST (1U + CONSOLE_RECORD_NAME_MAX + 6U)
#define CONSOLE_RECORD_MAX_PARTS 16U

static char record_dedup[CONSOLE_RECORD_DEDUP][CONSOLE_RECORD_LINE_MAX + 1U];
static size_t record_dedup_next;
static size_t record_dedup_count;
static char record_pending[CONSOLE_RECORD_PENDING][CONSOLE_RECORD_LINE_MAX + 1U];
static size_t record_pending_head;
static size_t record_pending_count;
static bool console_ready;

/*
 * ITEM-466 (round 5): the screen output budget.
 *
 * Every record still reaches the durable journal, but a text console gains
 * nothing from repainting an unbounded stream of routine records: each new line
 * shifts the whole body, and on 4K the owner saw the screen stutter and the
 * keyboard starve ("the keyboard practically does not type").  At most
 * CONSOLE_SCREEN_PAINT_PER_SECOND records are painted per second; the rest are
 * counted and the count is published on the `[PRODUCTION-LOOP]` heartbeat, so a
 * reader can tell "the screen is quieter" from "the machine stopped talking".
 * The clock is the 100 Hz tick, which is enough for a one-second window.
 */
#define CONSOLE_SCREEN_PAINT_PER_SECOND 12U
#define CONSOLE_SCREEN_WINDOW_TICKS 100U
static uint64_t console_screen_window_ticks;
static uint64_t console_screen_window_painted;
static uint64_t console_screen_suppressed;

/* ITEM-478 (round 16): how many service lines reached the serial stream and
   the durable journal without ever touching the screen.  The `[CONSOLE] boot`
   record publishes it, so a proof can tell "the markers still flow" from "the
   markers went nowhere". */
static uint64_t console_service_lines;

static bool console_screen_record_allow(void) {
    const uint64_t now = timer_ticks();

    if (now - console_screen_window_ticks >= CONSOLE_SCREEN_WINDOW_TICKS) {
        console_screen_window_ticks = now;
        console_screen_window_painted = 0U;
    }
    if (console_screen_window_painted >= CONSOLE_SCREEN_PAINT_PER_SECOND) {
        ++console_screen_suppressed;
        return false;
    }
    ++console_screen_window_painted;
    return true;
}

/* ITEM-405: defined below, painted by console_init() after its banner. */
static void console_record_flush(void);
static void console_record_paint(const char *line);

/*
 * Trigger a real Double Fault by making the Page Fault IDT gate
 * not-present and then causing a supervisor page fault.  The CPU will
 * attempt to deliver #PF, discover the gate is not-present, and raise
 * #DF instead.  This command does not return.
 */
/* VMM test/diagnostic hook */
extern uint64_t vmm_hhdm_offset(void);

static void cmd_doublefault(void) {
    console_write("TRIGGERING DOUBLE FAULT VIA #PF GATE NOT-PRESENT...\n");

    /* Make the Page Fault IDT gate not-present.  The next #PF will
     * fail delivery and escalate to a Double Fault. */
    interrupts_make_pf_gate_not_present();

    /* Now trigger a supervisor page fault.  The CPU will try to
     * deliver #PF to a not-present gate, which escalates to #DF. */
    volatile uint64_t *const address = (volatile uint64_t *)(uintptr_t)PAGE_FAULT_ADDRESS;
    *address = 0U;

    /* Unreachable. */
    for (;;) {}
}

#ifndef HFOC_CONSOLE_HOST_TEST
/*
 * ITEM-581: the `network` command paints the status report on the screen; the
 * console transcript carries the same answer to the durable journal, so
 * `bootlog` and the exported file can read it too.
 */
static void console_status_report_sink(void *context, const char *line) {
    uint64_t flags;

    (void)context;

    if (line == NULL) {
        return;
    }
    /*
     * ITEM-635 gate repair: the status report is one record, and an IRQ trace
     * line printed between its fragments made the QEMU proof see a truncated
     * `[RNG] ... state=` line and reject it.  Hold the serial sequence for the
     * whole line, exactly as the language answers already do.
     */
    flags = serial_sequence_begin();
    console_write(line);
    console_putc('\n');
    serial_sequence_end(flags);
}
#endif

static void cmd_appliance(void) {
    struct appliance_console_snapshot snapshot;
    char output[APPLIANCE_CONSOLE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    enum appliance_console_status config_status;
    const enum appliance_console_status snapshot_status =
        appliance_console_snapshot_init(
            &snapshot,
            appliance_identity_get(),
            APPLIANCE_CONSOLE_STATE_OK,
            timer_ticks());

    if (snapshot_status != APPLIANCE_CONSOLE_OK) {
        console_write("[APPLIANCE] SNAPSHOT FAILED\n");
        return;
    }
    config_status = appliance_console_publish_config(
        &snapshot,
        boot_persistence_published
            ? boot_config_source
            : APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING,
        boot_persistence_published
            ? boot_hostname
            : APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME,
        boot_persistence_published
            ? strlen(boot_hostname)
            : sizeof(APPLIANCE_CONFIG_VALIDATION_DEFAULT_HOSTNAME) - 1U);
    if (config_status != APPLIANCE_CONSOLE_OK) {
        console_write("[APPLIANCE] CONFIG FAILED\n");
        return;
    }
    if (appliance_console_render(
            &snapshot, output, sizeof(output), &output_length) ==
        APPLIANCE_CONSOLE_OK) {
        console_write(output);
    } else {
        console_write("[APPLIANCE] RENDER FAILED\n");
    }
}

static uint64_t cursor_x;
static uint64_t cursor_y;
static uint64_t usable_memory_mib;
static uint32_t boot_cpu_cores = 1U;
static char input_line[INPUT_MAX + 1U];
static size_t input_length;
static size_t input_cursor;
/*
 * ITEM-506: a character that did not fit the line is remembered, so an
 * over-long hidden line is refused as too long instead of being silently
 * truncated to the policy maximum and accepted as a shorter password.
 */
static bool input_overflow;
static size_t previous_rendered_length;
/*
 * ITEM-483 (round 20 follow-up): the one-cell typing fast path may only paint
 * while the prompt and the typed line still stand exactly where it left them on
 * both surfaces the operator reads.  Two kinds of writer break that:
 *
 *   * a screen paint that is not the input echo - a mirrored record, a body
 *     scroll, a menu paint, a command's own output, `clear` - moves the cursor
 *     the fast path did not leave; and
 *   * a record assembled for the serial stream, even a journal-only one that
 *     never touches the framebuffer, pushes the typed characters apart on the
 *     wire, which is how the 2026-09-26 load proof lost `e`/`f` of `abcdef`.
 *
 * `console_output_generation` counts both: every `console_emit()` that is not
 * the input echo (and not the deferred echo's replay), every record opened
 * through `console_mirror_open()`, and once by `clear_body()`, which paints the
 * framebuffer directly.  `console_input_line_generation` remembers the
 * generation the line was last rendered against.  The fast path requires the
 * two to be equal; when they are not, it does the full `redraw_input_line()`,
 * which re-emits the prompt and the whole line to both surfaces.
 */
static uint64_t console_output_generation;
static uint64_t console_input_line_generation;
static char history[HISTORY_CAPACITY][INPUT_MAX + 1U];
static size_t history_count;
static size_t history_oldest;
static bool history_navigation_active;
static size_t history_navigation_offset;
static char saved_draft[INPUT_MAX + 1U];
static size_t saved_draft_length;
static size_t saved_draft_cursor;
/* ITEM-419: the menu the operator drives from the machine's own keyboard. */
static struct local_menu menu_state;
/*
 * ITEM-447: the network record the Network entry writes, and the answer in
 * progress.  The binding is two words owned by the boot path; without it the
 * entry keeps publishing the record it always published.
 */
static struct console_network_binding network_binding;
static bool network_binding_published;
static struct local_network_setup network_setup;
/*
 * ITEM-430: the menu the machine offers when nobody asks for it.  `input_seen`
 * is the operator having pressed anything at all since boot, and
 * `menu_offer_open` is "the offer is painted and the next ENTER on an empty
 * line opens the menu".
 */
static struct local_menu_offer menu_offer;
static bool menu_offer_open;
static bool console_input_seen;
static bool admin_credentials_active;
static bool entropy_seed_active;

/*
 * ITEM-448: the pointer the mouse draws, and the menu lines a click can run.
 *
 * `pointer_now` is the console's own copy of where the arrow is; the driver
 * only sends deltas.  `pointer_targets` is rebuilt by every menu paint, so a
 * click hit-tests the lines that are actually on the screen - and a scroll or
 * a menu that leaves clears them, so a click can never run an entry that is no
 * longer there.  The arrow itself is owned by framebuffer.c, which saves what
 * it covers and puts it back.
 */
static struct pointer_state pointer_now;
static bool pointer_ready;
/* ITEM-484 (round 22): the readiness announcement paints the arrow exactly
   once, whether it arrives before or after the first decoded batch. */
static bool pointer_initial_shown;
/* ITEM-463: the pointer's own traffic and the cost of drawing the arrow. */
static uint64_t console_pointer_moves;
static uint64_t console_pointer_max_us;
static uint64_t console_pointer_window_us;
static uint64_t console_pointer_reported_moves;
static uint64_t console_pointer_reported_max_us;
static bool console_pointer_publishing;
/*
 * ITEM-472 (СРОЧНО-22): the window the once-a-second record describes.  `dx`/
 * `dy` are the applied deltas summed, `dmax` the largest single applied delta,
 * and `second` the wall-clock second the last record went out at.  The gain is
 * the operator's multiplier, set by `PTR`; 1 is the default and the
 * ITEM-471 arithmetic.
 */
static int64_t console_pointer_window_dx;
static int64_t console_pointer_window_dy;
static uint64_t console_pointer_window_dmax;
static uint32_t console_pointer_gain = 1U;
static uint64_t console_pointer_published_second;
/*
 * ITEM-480 (round 18): the pointer's turn budget and coalescing evidence.
 * `batches` counts the batches the console processed, `batch_events` the
 * decoded reports they consumed, and the two totals split the arrow's cost into
 * the drawing (`framebuffer_cursor_show`) and the observability barrier
 * (`framebuffer_sync`) that now runs once per batch.  `draws` is the existing
 * `console_pointer_moves`: a batch that moved drew once.
 */
static uint64_t console_pointer_batches;
static uint64_t console_pointer_batch_events;
static uint64_t console_pointer_draw_us_total;
static uint64_t console_pointer_sync_us_total;
/*
 * ITEM-482 (round 20): the input drain accounting.  `console_keyboard_echo_active`
 * marks the keyboard's event handling so the drains it causes are charged to
 * `kbd_sync_us`; `console_input_needs_sync` remembers that a pointer draw queued
 * write-combining stores that the single `console_end_input_batch()` still owes.
 */
static bool console_keyboard_echo_active;
static uint64_t console_keyboard_sync_us_total;
static bool console_input_needs_sync;
static struct pointer_target pointer_targets[LOCAL_MENU_ENTRY_MAX];
static size_t pointer_target_count;
/* Set while the geometry record is being written: the scroll that record may
   cause must not publish a second record from inside the first. */
static bool pointer_publishing;
/* The geometry the last `[POINTER] menu` record published, so a repaint that
   moved nothing stays quiet. */
static size_t pointer_published_targets;
static int32_t pointer_published_x;
static int32_t pointer_published_y;
static int32_t pointer_published_width;

/* The arrow is hidden around a text write - otherwise a record would be
   painted with an arrow baked into the middle of it - and shown again after. */
static void pointer_suspend(void) {
    if (framebuffer_cursor_visible()) {
        framebuffer_cursor_hide();
    }
}

static void pointer_resume(void) {
    if (pointer_ready && framebuffer_ready()) {
        (void)framebuffer_cursor_show((uint64_t)pointer_now.x,
                                      (uint64_t)pointer_now.y);
    }
}

/*
 * ITEM-484 (round 22): the pointer appears on mouse readiness, not on its
 * first movement and not on a click.
 *
 * The owner's hardware run left the screen without an arrow until the mouse
 * moved: `console_process_mouse_batch()` set `pointer_ready` but only called
 * `framebuffer_cursor_show()` inside its movement branch, and a zero report or
 * a bare button edge therefore left the surface untouched.  The main loop
 * calls this function every turn while `local_input_mouse_ready()` is true, so
 * the first turn after the controller binds a mouse centres the arrow and
 * paints it once; a click is never the mechanism that makes it appear.  The
 * deferred write-combining store is drained by the same input batch's single
 * `console_end_input_batch()`.
 */
void console_pointer_ready(void) {
    if (!framebuffer_ready() || pointer_initial_shown) {
        return;
    }
    if (!pointer_ready) {
        pointer_state_reset(&pointer_now, (int32_t)(framebuffer_width() / 2U),
                            (int32_t)(framebuffer_height() / 2U));
        pointer_ready = true;
    }
    pointer_initial_shown = true;
    (void)framebuffer_cursor_show((uint64_t)pointer_now.x,
                                  (uint64_t)pointer_now.y);
    console_input_needs_sync = true;
}

static void redraw_input_line(void);
static void write_u64(uint64_t value);
static void pointer_publish_targets(void);
static void pointer_scroll_targets(uint64_t cells);
/* ITEM-463: the pointer's own cost record, journal-only. */
static void console_publish_pointer(void);
static size_t console_offer_record_append(char *output, size_t capacity,
                                          size_t length, const char *text);
static size_t console_scroll_append_u64(char *output, size_t capacity,
                                        size_t length, uint64_t value);
/* ITEM-472: append a signed applied delta, sign included. */
static size_t console_pointer_append_signed(char *output, size_t capacity,
                                            size_t length, int64_t value);
static void reset_history_navigation_state(void);
static void reset_input_state(void);
static void console_run_command(const char *word);
/* ITEM-509 increment 3: the code-point painter the replay decodes into. */
static void console_emit_codepoint(uint32_t codepoint, const char *bytes,
                                   size_t length, bool to_serial,
                                   int invalid_byte);
static void console_emit_invalid_byte(unsigned char value);
/* ITEM-419: the menu leaves back to a prompt that is defined below it. */
static void prompt(void);
static void console_management_flush(void);
static void clear_body(void);
/* ITEM-448: a click runs the same entry the keyboard's ENTER runs. */
static void local_menu_run(enum local_menu_entry entry);

/*
 * ITEM-460: the durable transcript of the console itself.
 *
 * The owner's 2026-09-20 report was blunt: "I am tired of photographing the
 * screen after every run; cannot the screen output go to the log, so you can
 * read it afterwards without guessing".  The record funnel already hands every
 * published *record* to the boot log, but the console's own conversation -
 * the typed command and the answer it produced - went through `console_write()`
 * and reached the screen and the serial stream only.  On a machine whose only
 * output is the framebuffer that conversation existed for a few seconds and was
 * then gone, which is exactly how the `admin-provision` refusal was lost under
 * a stream of timer records.
 *
 * The transcript is bounded and journal-only: it never paints, so adding it
 * does not scroll the screen it exists to make unnecessary to photograph.  Each
 * command becomes `[CONSOLE] cmd="..."` and each output line becomes
 * `[CONSOLE] out="..."`; the removed `admin-provision <secret>` /
 * `admin-login <secret>` spellings are still journaled with their name and
 * `<redacted>` in place of the argument (and never echoed), because a shipped
 * manual can still send one there even though the names no longer run.
 *
 * The answer is buffered whole and journaled only when the command ends.
 * Journaling from inside the character painter - which is where the answer is
 * produced - re-entered the console's own paint and scroll state: the 2026-09-20
 * `test-usb-mouse-menu-qemu` run showed a journal write landing between the
 * pointer's geometry record and its own newline, so the menu moved a row and
 * the click hit the entry below the one the geometry named.  Deferring the
 * writes to `console_transcript_end()` keeps them outside any paint, at the
 * cost of a bounded 2 KiB buffer; an answer longer than that is truncated and
 * says so with one `<truncated>` record.
 */
#define CONSOLE_TRANSCRIPT_MAX CONSOLE_RECORD_MAX
#define CONSOLE_TRANSCRIPT_RECORD_MAX (CONSOLE_TRANSCRIPT_MAX + 32U)
#define CONSOLE_TRANSCRIPT_TOTAL_MAX 2048U
static char transcript_body[CONSOLE_TRANSCRIPT_TOTAL_MAX + 1U];
static size_t transcript_length;
static bool transcript_active;
static bool transcript_overflow;
static void console_journal_record(const char *text, size_t length);
#ifndef HFOC_CONSOLE_HOST_TEST
/* ITEM-557: the diagnostic refusal as one durable record, defined below.  The
   host build has no `diag save` command, so neither has the record. */
static void diag_failure_record(const struct diag_failure *failure);
#endif
/*
 * ITEM-478 (round 16): one service line, serial and durable journal only.
 *
 * The owner's 2026-09-25 review asked that the screen carry only the product
 * banner, the prompt, the operator's own typing and answers, the local menu
 * and an input error.  A service marker (`[LOCAL-MENU] open`,
 * `[POINTER] menu targets=`, the readiness lines) exists for the serial
 * capture and the durable journal, exactly as the periodic records ITEM-477
 * moved do, so every one of them goes through this single funnel: the serial
 * stream a proof reads keeps the line, `console_journal_record()` keeps it in
 * the log, and neither touches the framebuffer.  The readiness banner is
 * stored this way too, so a machine with no serial capture still gains the
 * facts from `bootlog` in the durable journal.
 */
static void console_service_line(const char *record, size_t length);
static void console_transcript_begin(void);
static void console_transcript_end(void);
static void console_transcript_command(const char *line);
static void console_transcript_capture(char value);

/*
 * ITEM-419: the local menu.  The appliance rendered a list of numbered actions
 * since ITEM-344 and nothing could select one, so the machine's own screen
 * offered a menu that did not respond.  The model lives in local_menu.c and is
 * proven on the host; this is the half that owns the screen, the keyboard and
 * the actions - and the actions live here because a reboot or a power off has
 * to publish what it is about to do and flush the boot log to the appliance's
 * own disk before the machine goes down.
 */

/*
 * ITEM-440: one menu line is one IRQ-safe write.
 *
 * The menu is painted character by character - console_write() hands each
 * character to console_putc(), which mirrors it to the serial port - so an IRQ
 * trace line (`[IOAPIC] irq0 ...`, `[LAPIC-TIMER] ...`) could land inside a
 * menu line and split the marker a proof waits for.  Observed on 2026-09-17 in
 * the `keyboard_absent` phase of test-qemu-production-storage-boot.sh as
 * `  [1] Stat` + `[IOAPIC] irq0 ...` + `us  (s)`, which the proof reads as a
 * missing marker; docs/ci-self-hosted-runner.md names the class ("a split
 * marker line is not a timeout").  The records already solve it with
 * serial_sequence_begin()/serial_sequence_end(), which is cpu_irq_save()/
 * cpu_irq_restore(); the paint does the same, so the operator's menu and the
 * proof's markers are one write.  The window is bounded by the rendered menu
 * and is entered only when the menu is offered or redrawn.
 */
static void console_write_atomic(const char *text) {
    const uint64_t flags = serial_sequence_begin();

    console_write(text);
    serial_sequence_end(flags);
}

/*
 * ITEM-509 increment 21: the console's offer and availability prose comes from
 * the one catalog in the console's published language; a key the catalog cannot
 * answer is a visible named refusal, never an English line in a Russian
 * console.  The machine records around it (`[LOCAL-MENU] offered ...`) stay
 * English on purpose.
 */
static const char *console_local_menu_text(const char *key) {
    const char *text =
        appliance_language_string(console_active_language(), key);

    return text != NULL ? text : "CONSOLE TRANSLATION MISSING\n";
}

/*
 * ITEM-509 increment 22: the console's field labels come from the same catalog
 * in the same published language.  A label is followed by its machine value on
 * the same line, so the fallback is visible but carries no newline of its own;
 * the values (`ON`/`OFF`, numbers, `REFUSED: ...`) stay English machine text.
 */
static const char *console_label_text(const char *key) {
    const char *text =
        appliance_language_string(console_active_language(), key);

    return text != NULL ? text : "CONSOLE TRANSLATION MISSING";
}

/*
 * ITEM-509 increment 24: the refusal the power command prints when the
 * platform declares no mechanism.  The label and the action name beside it are
 * machine text and stay English; the sentence itself is operator prose and
 * follows the chosen language through the local-menu helper, whose fallback
 * ends the line.  The kernel branch that calls this sits behind the
 * platform-power build guard, so the host proof calls this function directly
 * and drives the exact line the operator would see.
 */
static void console_power_refusal(const char *action) {
    console_write(console_label_text("console.label.power"));
    console_write(action);
    console_write(console_local_menu_text("console.prose.power_refused"));
}

static void local_menu_paint(void) {
    char rendered[LOCAL_MENU_RENDER_MAX + 1U];
    const size_t length = local_menu_render(
        &menu_state, console_active_language(), rendered,
        sizeof(rendered));

    pointer_target_count = 0U;
    pointer_suspend();
    if (length < sizeof(rendered)) {
        console_write_atomic(rendered);
    } else {
        console_write_atomic(
            console_local_menu_text("console.menu.render_failed"));
    }
    /*
     * ITEM-474: the menu's own lines may have opened a deferred batch.  Apply
     * it before the target rows are counted back from the cursor, or
     * `console_flush_deferred()` would later move the freshly placed
     * rectangles by the same rows a second time and every click would aim a
     * row too high.  The flush runs with `pointer_target_count == 0`, so it
     * moves nothing.
     */
    console_flush_deferred();
    /*
     * ITEM-448: the lines that were just painted are the rectangles a click can
     * run.  The render ends one line below the last of them, so the entry's own
     * row is counted back from the cursor: eight lines are painted (the title,
     * the six entries and the help line), and the entry at index i is the
     * (1 + i)-th of them.  A scroll during the paint moved every line together,
     * so the same arithmetic still names the row the operator can see.
     */
    {
        const size_t entries = local_menu_entry_count();
        const uint64_t width = framebuffer_width();

        if (entries <= LOCAL_MENU_ENTRY_MAX &&
            cursor_y >= (uint64_t)(entries + 2U) * console_cell_height &&
            width > LEFT) {
            for (size_t index = 0U; index < entries; ++index) {
                const uint64_t rows_below =
                    (uint64_t)(entries + 1U - index);

                pointer_targets[index].x = (int32_t)LEFT;
                pointer_targets[index].y =
                    (int32_t)(cursor_y - rows_below * console_cell_height);
                pointer_targets[index].width =
                    (int32_t)(width - LEFT);
                pointer_targets[index].height = (int32_t)console_cell_height;
                pointer_targets[index].value = (int32_t)index;
            }
            pointer_target_count = entries;
        }
    }
    /*
     * ITEM-448: the geometry a proof (or a person reading the log) needs to aim
     * a click: the column every entry shares, the row of the first one, the
     * line height and the width of the row.  It is published only when it
     * changes, so a redraw that moved nothing does not repeat it - and a
     * scrolled menu publishes the new place instead of leaving the old one
     * behind.
     */
    pointer_publish_targets();
    pointer_resume();
}

/*
 * ITEM-448: the geometry record.  It is emitted whenever the rectangles move -
 * a paint that put them somewhere new, or a scroll that carried them up a line
 * - so a reader can aim at the rows that are on the screen now.
 *
 * ITEM-478 (round 16): the record is a service marker, so it reaches the serial
 * stream and the durable journal and never the screen.  It therefore no longer
 * occupies a row itself and cannot scroll the menu it describes; the published
 * row is exactly the row `local_menu_paint()` placed.  A scroll caused by a
 * later painted line still republishes the moved geometry.
 */
static void pointer_publish_targets(void) {
    int32_t shown_y;

    if (pointer_target_count == 0U || pointer_publishing) {
        return;
    }
    shown_y = pointer_targets[0].y;
    {
        const int32_t row_width = pointer_targets[0].width;

        if (pointer_published_targets == pointer_target_count &&
            pointer_published_x == pointer_targets[0].x &&
            pointer_published_y == shown_y &&
            pointer_published_width == row_width) {
            return;
        }
        pointer_published_targets = pointer_target_count;
        pointer_published_x = pointer_targets[0].x;
        pointer_published_y = shown_y;
        pointer_published_width = row_width;
        pointer_publishing = true;
        {
            char geometry[128];
            size_t length = 0U;

            length = console_offer_record_append(
                geometry, sizeof(geometry), length, "[POINTER] menu targets=");
            length = console_scroll_append_u64(
                geometry, sizeof(geometry), length,
                (uint64_t)pointer_target_count);
            length = console_offer_record_append(geometry, sizeof(geometry),
                                                 length, " x=");
            length = console_scroll_append_u64(
                geometry, sizeof(geometry), length,
                (uint64_t)pointer_targets[0].x);
            length = console_offer_record_append(geometry, sizeof(geometry),
                                                 length, " y=");
            length = console_scroll_append_u64(geometry, sizeof(geometry),
                                               length, (uint64_t)shown_y);
            length = console_offer_record_append(geometry, sizeof(geometry),
                                                 length, " line=");
            length = console_scroll_append_u64(geometry, sizeof(geometry),
                                               length, console_cell_height);
            length = console_offer_record_append(geometry, sizeof(geometry),
                                                 length, " width=");
            length = console_scroll_append_u64(geometry, sizeof(geometry),
                                               length, (uint64_t)row_width);
            length = console_offer_record_append(geometry, sizeof(geometry),
                                                 length, "\n");
            console_service_line(geometry, length);
        }
        pointer_publishing = false;
    }
}

/*
 * ITEM-448: a scroll moved every painted line up by one, so the rectangles a
 * click was hit-testing move with them - the menu's text did.  A rectangle that
 * has left the body is dropped rather than left behind at a row it no longer
 * describes, and the new geometry is published, because a proof that aimed at
 * the old row would click a line the screen no longer shows there.
 */
static void pointer_scroll_targets(uint64_t cells) {
    size_t kept = 0U;
    size_t index;
    const int32_t moved_by = (int32_t)(cells * console_cell_height);

    if (pointer_target_count == 0U || cells == 0U) {
        return;
    }
    for (index = 0U; index < pointer_target_count; ++index) {
        const int32_t moved_y =
            pointer_targets[index].y - moved_by;

        if (moved_y + pointer_targets[index].height <= (int32_t)BODY_TOP) {
            continue;
        }
        pointer_targets[kept] = pointer_targets[index];
        pointer_targets[kept].y = moved_y;
        ++kept;
    }
    pointer_target_count = kept;
    pointer_publish_targets();
}

/*
 * ITEM-448: what a left click does.  The click is published with the position
 * it landed at and the counters behind it - a proof reads one line and knows
 * the pointer moved before the click - and when the menu is open and the click
 * landed on an entry's own row, that entry runs through exactly the path the
 * keyboard's ENTER uses.  A click anywhere else is published as a miss and
 * changes nothing.
 */
static void pointer_click(void) {
    int32_t hit = -1;
    char word[33];
    size_t word_length = 0U;

    if (local_menu_active(&menu_state) && pointer_target_count != 0U) {
        hit = pointer_hit_test(pointer_targets, pointer_target_count,
                               pointer_now.x, pointer_now.y);
    }
    {
        char click_record[128];
        size_t click_length = 0U;

        if (hit >= 0 && local_menu_select(&menu_state, (size_t)hit)) {
            const char *selected_word =
                local_menu_entry_word(local_menu_selected(&menu_state));

            while (selected_word[word_length] != '\0' && word_length < 32U) {
                word[word_length] = selected_word[word_length];
                ++word_length;
            }
            word[word_length] = '\0';
            click_length = console_offer_record_append(
                click_record, sizeof(click_record), click_length,
                "[POINTER] click entry=");
            click_length = console_offer_record_append(
                click_record, sizeof(click_record), click_length, word);
            click_length = console_offer_record_append(
                click_record, sizeof(click_record), click_length,
                " action=RUN");
        } else {
            click_length = console_offer_record_append(
                click_record, sizeof(click_record), click_length,
                "[POINTER] click entry=NONE action=NONE");
        }
        click_length = console_offer_record_append(
            click_record, sizeof(click_record), click_length, " x=");
        click_length = console_scroll_append_u64(
            click_record, sizeof(click_record), click_length,
            (uint64_t)pointer_now.x);
        click_length = console_offer_record_append(
            click_record, sizeof(click_record), click_length, " y=");
        click_length = console_scroll_append_u64(
            click_record, sizeof(click_record), click_length,
            (uint64_t)pointer_now.y);
        click_length = console_offer_record_append(
            click_record, sizeof(click_record), click_length, " moves=");
        click_length = console_scroll_append_u64(
            click_record, sizeof(click_record), click_length,
            (uint64_t)pointer_now.moves);
        click_length = console_offer_record_append(
            click_record, sizeof(click_record), click_length, " presses=");
        click_length = console_scroll_append_u64(
            click_record, sizeof(click_record), click_length,
            (uint64_t)pointer_now.presses);
        click_length = console_offer_record_append(
            click_record, sizeof(click_record), click_length, "\n");
        console_service_line(click_record, click_length);
    }
    if (hit >= 0) {
        /*
         * The entry runs through the keyboard's own path.  The menu stays open
         * for the entries that do not leave it - exactly as it does when the
         * operator presses ENTER - so a second click on a row that is still on
         * the screen still runs it.  An entry that does leave (Network's
         * wizard, Reboot, Shut down, a successful Recovery) clears the
         * rectangles in `local_menu_leave()`, so a click can never run a line
         * the screen no longer shows.
         */
        local_menu_run(local_menu_selected(&menu_state));
    }
}

/*
 * ITEM-448: one decoded mouse report from the driver.  The pointer starts in
 * the middle of the screen - a mouse that is bound but never moved still has a
 * visible pointer - moves by the report's deltas, and acts on the left
 * button's edges.  Where the pointer is stays the console's business: the
 * driver publishes deltas and counters, exactly as it publishes keys.
 */
/*
 * ITEM-448: where the arrow is, for the boot record and the heartbeat.  False
 * until the first mouse report placed it, so a machine without a pointer does
 * not publish a position that means nothing.
 */
bool console_pointer_position(int32_t *x, int32_t *y) {
    if (x != NULL) {
        *x = pointer_now.x;
    }
    if (y != NULL) {
        *y = pointer_now.y;
    }
    return pointer_ready;
}

/*
 * ITEM-463: the pointer's own cost record.  A mouse report can arrive on every
 * loop turn, so the record is a summary like the scroll's and is journal-only,
 * because a cost record that painted itself would pay the cost it exists to
 * publish.
 *
 * ITEM-471 (СРОЧНО-21): the record also names where the arrow is (`x=`/`y=`)
 * and how many movements ran into a screen edge (`clamped=`).  The owner's
 * round-9 log showed `moves=32` and no picture, and neither the position nor
 * the clamp was in the journal, so "the pointer moved" and "the pointer is
 * stuck in a corner" read the same.
 *
 * ITEM-472 (СРОЧНО-22): the record goes out once a real second, and it grew
 * the applied-delta sums (`dx=`/`dy=`), the largest single applied delta
 * (`dmax=`) and the operator's gain (`gain=`) so the next run separates a
 * device that sends tiny deltas from one whose reports are lost.  At its
 * widest every field fits in 256 bytes, inside the 320-byte journal line.
 */
#define CONSOLE_POINTER_RECORD_MAX 256U

static void console_publish_pointer(void) {
    char pointer_record[CONSOLE_POINTER_RECORD_MAX];
    size_t length = 0U;
    uint64_t window_moves;

    if (console_pointer_publishing || console_pointer_moves == 0U) {
        return;
    }
    console_pointer_publishing = true;
    window_moves = console_pointer_moves - console_pointer_reported_moves;
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, "[POINTER] moves=");
    length = console_scroll_append_u64(pointer_record, sizeof(pointer_record),
                                       length, console_pointer_moves);
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " us=");
    length = console_scroll_append_u64(pointer_record, sizeof(pointer_record),
                                       length,
                                       window_moves == 0U
                                           ? 0U
                                           : console_pointer_window_us /
                                                 window_moves);
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " max_us=");
    length = console_scroll_append_u64(pointer_record, sizeof(pointer_record),
                                       length, console_pointer_max_us);
    /* ITEM-471: where the arrow is and how often it hit an edge. */
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " x=");
    length = console_scroll_append_u64(pointer_record, sizeof(pointer_record),
                                       length,
                                       (uint64_t)(pointer_now.x < 0
                                                      ? 0
                                                      : pointer_now.x));
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " y=");
    length = console_scroll_append_u64(pointer_record, sizeof(pointer_record),
                                       length,
                                       (uint64_t)(pointer_now.y < 0
                                                      ? 0
                                                      : pointer_now.y));
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " clamped=");
    length = console_scroll_append_u64(pointer_record, sizeof(pointer_record),
                                       length, pointer_now.clamped);
    /*
     * ITEM-472 (СРОЧНО-22): the numbers that separate "the device sends tiny
     * deltas" from "the reports are lost".  `dx`/`dy` are the applied deltas
     * summed over the window and `dmax` is the largest single one, so a run
     * with `mouse_reports` climbing and `dx=0` names the device, while a large
     * `dmax` names a device that is answering.
     */
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " dx=");
    length = console_pointer_append_signed(pointer_record,
                                           sizeof(pointer_record), length,
                                           console_pointer_window_dx);
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " dy=");
    length = console_pointer_append_signed(pointer_record,
                                           sizeof(pointer_record), length,
                                           console_pointer_window_dy);
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " dmax=");
    length = console_scroll_append_u64(pointer_record, sizeof(pointer_record),
                                       length, console_pointer_window_dmax);
    length = console_offer_record_append(pointer_record, sizeof(pointer_record),
                                         length, " gain=");
    length = console_scroll_append_u64(pointer_record, sizeof(pointer_record),
                                       length, console_pointer_gain);
    if (length + 1U < sizeof(pointer_record)) {
        console_publish_journal(pointer_record, length);
        /*
         * ITEM-480 (round 18): the pointer's coalescing evidence, beside the
         * record it explains.  `events` is how many decoded reports the window
         * consumed, `draws` how many times the arrow was painted, so a run
         * where the movement coalesced shows `draws` far below `events`;
         * `draw_us`/`sync_us` are the averages of the two halves the batch
         * measured separately; `budget` is the named per-turn bound.
         */
        {
            char batch_record[CONSOLE_POINTER_RECORD_MAX];
            size_t batch_length = 0U;
            const uint64_t draws = console_pointer_moves;

            batch_length = console_offer_record_append(
                batch_record, sizeof(batch_record), batch_length,
                "[POINTER-BATCH] draws=");
            batch_length = console_scroll_append_u64(
                batch_record, sizeof(batch_record), batch_length, draws);
            batch_length = console_offer_record_append(
                batch_record, sizeof(batch_record), batch_length, " events=");
            batch_length = console_scroll_append_u64(
                batch_record, sizeof(batch_record), batch_length,
                console_pointer_batch_events);
            batch_length = console_offer_record_append(
                batch_record, sizeof(batch_record), batch_length, " batches=");
            batch_length = console_scroll_append_u64(
                batch_record, sizeof(batch_record), batch_length,
                console_pointer_batches);
            batch_length = console_offer_record_append(
                batch_record, sizeof(batch_record), batch_length, " draw_us=");
            batch_length = console_scroll_append_u64(
                batch_record, sizeof(batch_record), batch_length,
                draws == 0U ? 0U : console_pointer_draw_us_total / draws);
            batch_length = console_offer_record_append(
                batch_record, sizeof(batch_record), batch_length, " sync_us=");
            batch_length = console_scroll_append_u64(
                batch_record, sizeof(batch_record), batch_length,
                draws == 0U ? 0U : console_pointer_sync_us_total / draws);
            batch_length = console_offer_record_append(
                batch_record, sizeof(batch_record), batch_length, " budget=");
            batch_length = console_scroll_append_u64(
                batch_record, sizeof(batch_record), batch_length,
                LOCAL_INPUT_MOUSE_DRAW_BUDGET);
            batch_length = console_offer_record_append(
                batch_record, sizeof(batch_record), batch_length, "\n");
            if (batch_length + 1U < sizeof(batch_record)) {
                console_service_line(batch_record, batch_length);
            }
        }
        console_pointer_reported_moves = console_pointer_moves;
        console_pointer_reported_max_us = console_pointer_max_us;
        console_pointer_window_us = 0U;
        console_pointer_window_dx = 0;
        console_pointer_window_dy = 0;
        console_pointer_window_dmax = 0U;
    }
    console_pointer_publishing = false;
}

/*
 * ITEM-472: the once-a-second pointer record, driven by the honest clock.
 *
 * The old record was published every 64 movements, and the heartbeat that
 * called it counted PIT *interrupts* - so a machine that lost ticks published
 * it once every several real seconds.  `timer_real_seconds()` is the PIT-
 * calibrated wall clock, so the gate is one real second whatever the tick
 * counter did.  The first movement still publishes at once, so a short session
 * is not silent.
 */
void console_pointer_tick(void) {
    const uint64_t second = timer_real_seconds();

    if (console_pointer_moves == 0U ||
        second == console_pointer_published_second) {
        return;
    }
    console_pointer_published_second = second;
    console_publish_pointer();
}

/*
 * ITEM-472: `PTR` shows the operator's gain and `PTR <n>` sets it.
 *
 * The owner tunes the feel in one hardware run instead of another code round:
 * the value multiplies the scaled delta, `[POINTER] gain=` records it, and the
 * default 1 is the arithmetic the last two rounds proved.  The answer is a
 * console command answer, so it reaches the screen, the serial stream and the
 * transcript.
 */
static void console_pointer_command(const char *line) {
    const char *argument = line;
    uint32_t value = 0U;
    bool has_digits = false;

    while (*argument != '\0' && *argument != ' ' && *argument != '\t') {
        ++argument;
    }
    while (*argument == ' ' || *argument == '\t') {
        ++argument;
    }
    while (*argument >= '0' && *argument <= '9') {
        has_digits = true;
        value = value * 10U + (uint32_t)(*argument - '0');
        if (value > POINTER_GAIN_MAX) {
            value = POINTER_GAIN_MAX;
        }
        ++argument;
    }
    if (has_digits) {
        console_pointer_gain = value == 0U ? 1U : value;
    }
    console_write("[POINTER] gain=");
    write_u64(console_pointer_gain);
    if (!has_digits) {
        console_write(" (PTR <1..");
        write_u64(POINTER_GAIN_MAX);
        console_write(console_label_text("console.prose.ptr_range_suffix"));
    }
    console_putc('\n');
}

/*
 * ITEM-473 (СРОЧНО-23): `ARM` shows and `ARM <1..16>` sets the mouse's
 * armed-transfer depth.
 *
 * The round-11 hardware run could not be explained from the journal alone: the
 * pointer's queue was full (`armed=16`) and its counters had frozen.  The
 * worker's diagnosis named the event ring - sixteen completions filled a
 * sixteen-TRB ring - and this command lets the owner check the remaining
 * question in one run instead of another code round: set the depth back to the
 * three that moved the pointer in ITEM-469, or leave sixteen, and watch the
 * `mouse_armed=`/`ringfull=` fields in the durable record.  The clamp belongs
 * to the driver, so a value outside the range answers the range's end.
 */
static void console_arm_command(const char *line) {
    const char *argument = line;
    uint32_t value = 0U;
    bool has_digits = false;

    while (*argument != '\0' && *argument != ' ' && *argument != '\t') {
        ++argument;
    }
    while (*argument == ' ' || *argument == '\t') {
        ++argument;
    }
    while (*argument >= '0' && *argument <= '9') {
        has_digits = true;
        value = value * 10U + (uint32_t)(*argument - '0');
        if (value > XHCI_ARM_DEPTH_MAX) {
            value = XHCI_ARM_DEPTH_MAX;
        }
        ++argument;
    }
    if (has_digits) {
        xhci_set_mouse_arm_depth(value);
    }
    console_write("[INPUT-ARM] mouse_arm_depth=");
    write_u64(xhci_mouse_arm_depth());
    console_write(" mouse_armed=");
    write_u64(xhci_mouse_armed_count());
    console_write(" event_ring=");
    write_u64(XHCI_EVENT_TRBS);
    if (!has_digits) {
        console_write(" (ARM <1..");
        write_u64(XHCI_ARM_DEPTH_MAX);
        console_write(console_label_text("console.prose.arm_range_suffix"));
    }
    console_putc('\n');
}

/*
 * ITEM-480 (round 18): the console consumes the pointer one batch at a time.
 *
 * A batch is already coalesced by `local_input`: a run of movements arrives as
 * one `dx`/`dy` sum and draws once, while a button edge arrives alone and is
 * applied in order.  The observability `framebuffer_sync()` therefore runs once
 * per batch - not once per decoded report - and the two costs are measured
 * separately (`draw_us=` and `sync_us=`) so the next record names which half of
 * the arrow's cost is the drawing.
 */
/* ITEM-621: the wheel notch reaches the retained history, not only a batch. */
static void pager_wheel(int32_t notches);

void console_process_mouse_batch(const struct usb_hid_mouse_batch *event) {
    if (event == NULL || !framebuffer_ready()) {
        return;
    }
    if (!pointer_ready) {
        pointer_state_reset(&pointer_now, (int32_t)(framebuffer_width() / 2U),
                            (int32_t)(framebuffer_height() / 2U));
        pointer_ready = true;
    }
    ++console_pointer_batches;
    console_pointer_batch_events += event->events;
    if (event->dx != 0 || event->dy != 0) {
        const struct pointer_bounds bounds = {
            (int32_t)framebuffer_width(),
            (int32_t)framebuffer_height()
        };
        const int32_t applied_dx =
            pointer_apply_delta(event->dx, console_scale, console_pointer_gain);
        const int32_t applied_dy =
            pointer_apply_delta(event->dy, console_scale, console_pointer_gain);

        /*
         * ITEM-470: the mode's own glyph scale - the number the `[FB]` record
         * publishes - decides how far one report's delta moves the arrow, so a
         * 4K interface three times the size no longer makes the pointer crawl.
         * ITEM-472: the console applies the delta itself so the applied sums
         * `dx`/`dy` and the largest single delta `dmax` are exactly what the
         * pointer received.
         */
        if (pointer_move(&pointer_now, applied_dx, applied_dy, bounds)) {
            const uint64_t started = timer_cycles();
            uint64_t draw_us;
            uint64_t micros;
            uint64_t magnitude;

            (void)framebuffer_cursor_show((uint64_t)pointer_now.x,
                                          (uint64_t)pointer_now.y);
            draw_us = timer_cycles_to_us(timer_cycles() - started);
            /*
             * ITEM-482 (round 20): the draw's write-combining stores are not
             * drained here.  They wait for the single
             * `console_end_input_batch()` after the whole input batch, so a
             * turn with three coalesced draws pays one drain, not three; the
             * pointer's `sync_us` average is charged there.
             */
            console_input_needs_sync = true;
            micros = draw_us;
            ++console_pointer_moves;
            console_pointer_window_us += micros;
            console_pointer_draw_us_total += draw_us;
            if (micros > console_pointer_max_us) {
                console_pointer_max_us = micros;
            }
            console_pointer_window_dx += (int64_t)applied_dx;
            console_pointer_window_dy += (int64_t)applied_dy;
            magnitude = (uint64_t)(applied_dx < 0 ? -applied_dx : applied_dx);
            if ((uint64_t)(applied_dy < 0 ? -applied_dy : applied_dy) >
                magnitude) {
                magnitude = (uint64_t)(applied_dy < 0 ? -applied_dy
                                                      : applied_dy);
            }
            if (magnitude > console_pointer_window_dmax) {
                console_pointer_window_dmax = magnitude;
            }
            /*
             * The first movement publishes at once so a short session is not
             * silent; every later one waits for `console_pointer_tick()`'s
             * real second.
             */
            if (console_pointer_reported_moves == 0U) {
                console_pointer_published_second = timer_real_seconds();
                console_publish_pointer();
            }
        }
    }
    if ((event->pressed & USB_HID_MOUSE_BUTTON_LEFT) != 0U) {
        (void)pointer_button(&pointer_now, true);
    }
    if ((event->released & USB_HID_MOUSE_BUTTON_LEFT) != 0U) {
        (void)pointer_button(&pointer_now, false);
        pointer_click();
    }
    /*
     * ITEM-621: the decoded wheel delta used to stop here.  The batch now
     * hands it to the same bounded scrollback the `scroll` command paints, so
     * one notch moves the viewport and a coalesced movement+wheel batch does
     * both without a second decode or a raw framebuffer shift.
     */
    if (event->wheel != 0) {
        pager_wheel(event->wheel);
    }
}

void console_process_mouse_event(const struct usb_hid_mouse_event *event) {
    struct usb_hid_mouse_batch batch;

    if (event == NULL) {
        return;
    }
    /*
     * ITEM-480: the single-report entry point stays for the callers that have
     * one decoded report in hand (and for the host proof); it is the same
     * arithmetic as a one-report batch.
     */
    batch.dx = event->dx;
    batch.dy = event->dy;
    batch.wheel = event->wheel;
    batch.pressed = event->pressed;
    batch.released = event->released;
    batch.events = (event->dx != 0 || event->dy != 0 || event->wheel != 0 ||
                    event->pressed != 0 || event->released != 0)
                       ? 1U
                       : 0U;
    console_process_mouse_batch(&batch);
}

/*
 * ITEM-447: the four questions the machine asks when the operator picks the
 * Network entry.  The console owns the record and the commit; the module owns
 * the questions and the validation, so the host test drives the whole answer
 * without a keyboard.
 *
 * ITEM-496: every question and every record is one IRQ-safe write, and every
 * record also reaches the durable journal.  The questions used the plain
 * writer, and the 2026-09-29 QEMU run showed what that costs: `[IOAPIC] irq1`
 * trace lines split `gateway: ` in the serial capture, so the gesture could
 * not be read back - the same class ITEM-440 fixed for the menu lines.  A
 * record only the screen ever sees cannot be read back from a hardware run
 * either, so the offer, the answer and every refusal go through the journal
 * funnel as well.
 */
#define CONSOLE_NETWORK_CURRENT_MAX 64U

static void console_network_setup_publish_record(
    const char *record,
    size_t length) {
    console_write_atomic(record);
    console_publish_journal(record, length);
}

static void console_network_setup_current_text(
    char *output,
    size_t capacity,
    size_t *output_length) {
    static const char unavailable[] = "UNAVAILABLE";
    size_t length = 0U;

    if (output == NULL || output_length == NULL || capacity == 0U) {
        return;
    }
    output[0] = '\0';
    if (network_binding_published && network_binding.current != NULL &&
        network_binding.current(
            network_binding.context, output, capacity, &length) &&
        length != 0U && length < capacity) {
        *output_length = length;
        return;
    }
    length = 0U;
    while (unavailable[length] != '\0' && length + 1U < capacity) {
        output[length] = unavailable[length];
        ++length;
    }
    output[length] = '\0';
    *output_length = length;
}

static void console_network_setup_publish(
    bool is_static,
    const struct network_ipv4_config *config) {
    char record[LOCAL_NETWORK_SETUP_RECORD_MAX];
    char current[CONSOLE_NETWORK_CURRENT_MAX];
    size_t length = 0U;
    size_t current_length = 0U;
    bool persisted = false;

    console_network_setup_current_text(
        current, sizeof(current), &current_length);
    if (!network_binding_published) {
        if (local_network_setup_unavailable(
                record, sizeof(record), &length) ==
            LOCAL_NETWORK_SETUP_OK) {
            console_network_setup_publish_record(record, length);
        }
        return;
    }
    persisted = network_binding.commit(
        network_binding.context, is_static, config);
    if (local_network_setup_record(
            is_static, config, persisted, current, current_length,
            record, sizeof(record), &length) == LOCAL_NETWORK_SETUP_OK) {
        console_network_setup_publish_record(record, length);
    }
}

static void console_network_setup_prompt(void) {
    char prompt_text[LOCAL_NETWORK_SETUP_PROMPT_MAX];
    size_t length = 0U;

    if (local_network_setup_prompt(
            &network_setup, prompt_text, sizeof(prompt_text), &length) ==
        LOCAL_NETWORK_SETUP_OK) {
        /*
         * ITEM-496: one question is one IRQ-safe write.  The operator reads it
         * while the keyboard IRQs paint their own trace lines, and a marker a
         * reader (or a proof) cannot find is a question the operator cannot
         * answer.
         */
        console_write_atomic(prompt_text);
    }
}

static void console_network_setup_start(void) {
    char current[CONSOLE_NETWORK_CURRENT_MAX];
    char record[LOCAL_NETWORK_SETUP_RECORD_MAX];
    size_t current_length = 0U;
    size_t record_length = 0U;

    /*
     * ITEM-496: the operator is told what the interface has before being
     * asked to change it, and the reading is durable - a hardware run that
     * answers a question has to show which question and which starting point.
     */
    console_network_setup_current_text(
        current, sizeof(current), &current_length);
    local_network_setup_begin(&network_setup);
    if (local_network_setup_offer(
            current, current_length, record, sizeof(record),
            &record_length) == LOCAL_NETWORK_SETUP_OK) {
        console_network_setup_publish_record(record, record_length);
    }
    console_network_setup_prompt();
}

static void console_network_setup_enter(void) {
    enum local_network_setup_action action = LOCAL_NETWORK_SETUP_ACTION_NONE;
    struct network_ipv4_config config;
    char output[LOCAL_NETWORK_SETUP_RECORD_MAX];
    size_t length = 0U;
    enum local_network_setup_status status;

    memset(&config, 0, sizeof(config));
    status = local_network_setup_feed(
        &network_setup, input_line, input_length, &action,
        output, sizeof(output), &length, &config);
    reset_input_state();
    if (status != LOCAL_NETWORK_SETUP_OK) {
        static const char internal_record[] =
            "[PRODUCTION-NET-SET] state=REFUSED reason=SETUP_INTERNAL\n";

        console_network_setup_publish_record(
            internal_record, sizeof(internal_record) - 1U);
        prompt();
        return;
    }
    switch (action) {
    case LOCAL_NETWORK_SETUP_ACTION_PROMPT:
        console_write_atomic(output);
        return;
    case LOCAL_NETWORK_SETUP_ACTION_COMMIT_DHCP:
        console_network_setup_publish(false, NULL);
        break;
    case LOCAL_NETWORK_SETUP_ACTION_COMMIT_STATIC:
        console_network_setup_publish(true, &config);
        break;
    case LOCAL_NETWORK_SETUP_ACTION_CANCEL: {
        static const char cancelled_record[] =
            "[PRODUCTION-NET-SET] state=CANCELLED\n";

        console_network_setup_publish_record(
            cancelled_record, sizeof(cancelled_record) - 1U);
        break;
    }
    case LOCAL_NETWORK_SETUP_ACTION_REFUSED:
        console_network_setup_publish_record(output, length);
        break;
    default:
        break;
    }
    prompt();
}

static void local_menu_leave(void) {
    local_menu_close(&menu_state);
    /* ITEM-448: the menu is gone, so a click has nothing left to run - and
       the geometry record starts over, so reopening the menu at the same place
       still publishes where its lines are. */
    pointer_target_count = 0U;
    pointer_published_targets = 0U;
    console_service_line("[LOCAL-MENU] closed\n",
                         strlen("[LOCAL-MENU] closed\n"));
    prompt();
    reset_history_navigation_state();
    reset_input_state();
}

static void local_menu_run(enum local_menu_entry entry) {
    const char *word = local_menu_entry_word(entry);

    /*
     * ITEM-443: the action line is a proof marker too ("run action=..."), and
     * ITEM-478: it is a service marker, so the same composed line goes to the
     * serial stream and the durable journal and never to the screen.
     */
    {
        static const char marker[] = "[LOCAL-MENU] run action=";
        char action_record[64];
        size_t action_length = sizeof(marker) - 1U;
        size_t word_length = strlen(word);

        if (action_length + word_length + 1U >= sizeof(action_record)) {
            word_length = sizeof(action_record) - action_length - 2U;
        }
        memcpy(action_record, marker, action_length);
        memcpy(action_record + action_length, word, word_length);
        action_length += word_length;
        action_record[action_length++] = '\n';
        action_record[action_length] = '\0';
        console_service_line(action_record, action_length);
    }
    switch (entry) {
    case LOCAL_MENU_ENTRY_STATUS:
        console_run_command("appliance");
        /* ITEM-581: the Status page also answers what the network is doing,
           address or no address. */
        console_run_command("network");
        break;
    case LOCAL_MENU_ENTRY_NETWORK:
        if (network_binding_published) {
            /*
             * ITEM-447: the menu hands the keyboard back to the line editor and
             * asks the four questions.  Leaving the menu is the same act as
             * ESC: the machine says so and paints a prompt, and the answer
             * publishes its own record.
             */
            local_menu_leave();
            console_network_setup_start();
            break;
        }
        /*
         * ITEM-496: a machine whose persistent record is not loaded cannot
         * store an answer, so the entry names that refusal - and the reason -
         * instead of running a status screen the operator did not ask for.
         * The record goes to the screen and the durable journal like every
         * other answer record; the menu stays open, so the operator can pick
         * another entry.
         */
        {
            char unavailable[LOCAL_NETWORK_SETUP_RECORD_MAX];
            size_t unavailable_length = 0U;

            if (local_network_setup_unavailable(
                    unavailable, sizeof(unavailable),
                    &unavailable_length) == LOCAL_NETWORK_SETUP_OK) {
                console_network_setup_publish_record(
                    unavailable, unavailable_length);
            }
        }
        break;
    case LOCAL_MENU_ENTRY_DIAGNOSTICS:
        console_run_command("kbd");
        console_run_command("pmm");
        break;
#ifndef HFOC_CONSOLE_HOST_TEST
    case LOCAL_MENU_ENTRY_REBOOT: {
        struct platform_reset_plan plan;

        if (platform_reset_plan_current(&plan) != PLATFORM_RESET_OK) {
            console_service_line(
                "[LOCAL-MENU] reboot refused: no reset plan\n",
                strlen("[LOCAL-MENU] reboot refused: no reset plan\n"));
            break;
        }
        appliance_boot_log_store_flush("reboot");
        console_service_line("[LOCAL-MENU] reboot now\n",
                             strlen("[LOCAL-MENU] reboot now\n"));
        platform_reset_execute(&plan);
        break;
    }
    case LOCAL_MENU_ENTRY_SHUTDOWN: {
        struct platform_power_plan plan;
        const enum platform_power_status status =
            platform_power_plan_current(PLATFORM_POWER_ACTION_SHUTDOWN, &plan);

        if (status != PLATFORM_POWER_OK) {
            static const char refusal[] = "[LOCAL-MENU] shutdown refused: ";
            const char *reason = platform_power_status_string(status);
            char refusal_record[96];
            size_t refusal_length = 0U;

            while (refusal[refusal_length] != '\0' &&
                   refusal_length + 1U < sizeof(refusal_record)) {
                refusal_record[refusal_length] = refusal[refusal_length];
                ++refusal_length;
            }
            for (size_t index = 0U; reason[index] != '\0' &&
                     refusal_length + 1U < sizeof(refusal_record);
                 ++index) {
                refusal_record[refusal_length++] = reason[index];
            }
            if (refusal_length + 1U < sizeof(refusal_record)) {
                refusal_record[refusal_length++] = '\n';
            }
            refusal_record[refusal_length] = '\0';
            console_service_line(refusal_record, refusal_length);
            break;
        }
        appliance_boot_log_store_flush("poweroff");
        console_service_line("[LOCAL-MENU] power off now\n",
                             strlen("[LOCAL-MENU] power off now\n"));
        platform_power_execute(&plan);
        break;
    }
#else
    case LOCAL_MENU_ENTRY_REBOOT:
    case LOCAL_MENU_ENTRY_SHUTDOWN:
        /* The host test drives the model, not the machine. */
        break;
#endif
    case LOCAL_MENU_ENTRY_RECOVERY:
        /*
         * The recovery path needs the sealed recovery volume and its own
         * confirmation, and neither is bound to this menu: the entry says so
         * instead of pretending to act.
         */
        console_service_line(
            "[LOCAL-MENU] recovery UNAVAILABLE on this build\n",
            strlen("[LOCAL-MENU] recovery UNAVAILABLE on this build\n"));
        break;
    default:
        break;
    }
    if (local_menu_active(&menu_state)) {
        local_menu_paint();
    }
}

static void local_menu_handle_event(const struct keyboard_event *event) {
    const enum local_menu_outcome outcome =
        local_menu_handle(&menu_state, event);

    switch (outcome) {
    case LOCAL_MENU_OUTCOME_REDRAW:
        local_menu_paint();
        break;
    case LOCAL_MENU_OUTCOME_RUN: {
        const enum local_menu_entry entry = local_menu_selected(&menu_state);

        local_menu_run(entry);
        break;
    }
    case LOCAL_MENU_OUTCOME_CLOSE:
        local_menu_leave();
        break;
    case LOCAL_MENU_OUTCOME_NONE:
    default:
        break;
    }
}

static void cmd_local_menu(void) {
    local_menu_open(&menu_state);
    local_menu_paint();
    /*
     * ITEM-419: the serial record of the menu opening, symmetric with the
     * `[LOCAL-MENU] closed` record the leave path writes, and the marker the
     * QEMU proof waits for.  It is written after the paint, so a reader - or a
     * proof - that sees the record has already seen the menu it announced.
     */
    console_service_line("[LOCAL-MENU] open\n",
                         strlen("[LOCAL-MENU] open\n"));
}

/*
 * ITEM-455: the bound of the composed offer record and the composer itself.
 * The record is one atomic write, so it is composed in a buffer first: its
 * longest shape is the read-only line, the reason word, `entries=6 keys=` and
 * the whole key list.
 */
#define LOCAL_MENU_OFFER_RECORD_MAX 128U

static size_t console_offer_record_append(
    char *output,
    size_t capacity,
    size_t length,
    const char *text) {
    while (*text != '\0' && length + 1U < capacity) {
        output[length++] = *text++;
    }
    if (capacity != 0U) {
        output[length] = '\0';
    }
    return length;
}

/*
 * ITEM-460: the transcript, published one bounded record per output line.
 *
 * The answer is buffered as it is painted and published here, after the
 * command has finished and no paint is in progress.  A line is bounded by the
 * same `console_journal_record()` sanitiser the painted mirror uses; a partial
 * last line is published too, and an answer that filled the buffer ends with
 * one `<truncated>` record instead of silently losing its tail.
 */
static void console_transcript_publish(void) {
    size_t start = 0U;

    for (size_t index = 0U; index <= transcript_length; ++index) {
        if (index == transcript_length || transcript_body[index] == '\n') {
            if (index > start) {
                char record[CONSOLE_TRANSCRIPT_RECORD_MAX];
                size_t length = 0U;

                length = console_offer_record_append(
                    record, sizeof(record), length, "[CONSOLE] out=\"");
                for (size_t position = start;
                     position < index && length + 1U < sizeof(record);
                     ++position) {
                    record[length++] = transcript_body[position];
                }
                if (length + 1U < sizeof(record)) {
                    record[length++] = '"';
                }
                record[length] = '\0';
                console_journal_record(record, length);
            }
            start = index + 1U;
        }
    }
    if (transcript_overflow) {
        console_journal_record("[CONSOLE] out=\"<truncated>\"",
                               strlen("[CONSOLE] out=\"<truncated>\""));
    }
    transcript_length = 0U;
    transcript_body[0] = '\0';
    transcript_overflow = false;
}

static void console_transcript_begin(void) {
    transcript_length = 0U;
    transcript_body[0] = '\0';
    transcript_overflow = false;
    transcript_active = true;
}

static void console_transcript_end(void) {
    transcript_active = false;
    console_transcript_publish();
}

static void console_transcript_capture(char value) {
    if (!transcript_active) {
        return;
    }
    if (value == '\n' || value == '\r') {
        if (transcript_length < CONSOLE_TRANSCRIPT_TOTAL_MAX) {
            transcript_body[transcript_length++] = '\n';
            transcript_body[transcript_length] = '\0';
        } else {
            transcript_overflow = true;
        }
        return;
    }
    if (value == '\b') {
        if (transcript_length > 0U) {
            --transcript_length;
            transcript_body[transcript_length] = '\0';
        }
        return;
    }
    if ((unsigned char)value < 0x20U || (unsigned char)value > 0x7EU) {
        return;
    }
    if (transcript_length < CONSOLE_TRANSCRIPT_TOTAL_MAX) {
        transcript_body[transcript_length++] = value;
        transcript_body[transcript_length] = '\0';
    } else {
        transcript_overflow = true;
    }
}

/*
 * ITEM-460/ITEM-506: the account command family takes no argument, so an
 * ordinary command is journaled whole.  The two removed names are the one
 * exception, and it is a safety net, not a command: the printed and shipped
 * manuals told the operator `admin-provision <SECRET>` for months, and an
 * operator who types it from memory would otherwise push a password into the
 * durable command record, the `UNKNOWN COMMAND` echo and the input history.
 * The bytes after the removed name are still a secret, so those three paths
 * strip them; the names still do not run.  The comparison folds ASCII case
 * through the same helper the console uses everywhere else.
 */
static bool console_legacy_secret_command(const char *line) {
    return console_command_prefix(line, "admin-provision ") ||
           console_command_prefix(line, "admin-login ");
}

static const char *console_legacy_secret_redact(const char *line) {
    if (console_command_prefix(line, "admin-provision ")) {
        return "admin-provision <redacted>";
    }
    if (console_command_prefix(line, "admin-login ")) {
        return "admin-login <redacted>";
    }
    return line;
}

static void console_transcript_command(const char *line) {
    char record[CONSOLE_TRANSCRIPT_RECORD_MAX];
    const char *safe = console_legacy_secret_redact(line);
    size_t length = 0U;

    length = console_offer_record_append(record, sizeof(record), length,
                                         "[CONSOLE] cmd=\"");
    length = console_offer_record_append(record, sizeof(record), length, safe);
    length = console_offer_record_append(record, sizeof(record), length, "\"");
    console_journal_record(record, length);
}

/*
 * ITEM-430: the machine offers its own menu.
 *
 * The menu is otherwise reachable only by typing `menu`, and the 2026-09-16
 * hardware run showed what that costs: on a machine whose local input is
 * unavailable the choices exist on the screen but nothing can ask for them.
 * This is the console's half - the decision is `local_menu_offer_due()` in
 * local_menu.c, the paint is the menu the operator already knows - and it
 * publishes which of the two offers it made, so a reader can tell an idle
 * keyboard from a machine that has none.
 */
void console_offer_local_menu(uint32_t now_ms) {
    /*
     * ITEM-598: a hidden credential or entropy line owns the operator's next
     * keystroke.  Painting an idle menu into that conversation obscured the
     * question and could turn an intended ENTER into a menu action, so the
     * offer waits until the hidden conversation closes.
     */
    if (admin_credentials_active || entropy_seed_active) {
        return;
    }
    const enum local_menu_offer_outcome outcome = local_menu_offer_due(
        &menu_offer, now_ms, console_ready, local_input_available,
        console_input_seen, local_menu_active(&menu_state));

    if (outcome == LOCAL_MENU_OFFER_NONE) {
        return;
    }
    char keys[LOCAL_MENU_KEY_LIST_MAX + 1U];
    char record[LOCAL_MENU_OFFER_RECORD_MAX];
    size_t length = 0U;

    local_menu_paint();
    /*
     * ITEM-455: the record names the keys the menu answers to, and the idle
     * offer names the key that opens the network editor - the one place the
     * static address is set.  The owner's run had no `[LOCAL-MENU]` record at
     * all and the offer named only ENTER, so the gesture that sets an address
     * was visible only after someone opened the menu to look for it.
     *
     * Both lines stay one write: ITEM-443/449 made every offer marker atomic
     * because the QEMU proofs wait for the whole line, and composing the
     * record in a buffer keeps that even though the keys are now a value.
     */
    if (local_menu_key_list(keys, sizeof(keys)) == 0U) {
        keys[0] = '\0';
    }
    if (outcome == LOCAL_MENU_OFFER_NO_LOCAL_INPUT) {
        /*
         * ITEM-449: ITEM-443 made the menu's records IRQ-safe, but the
         * read-only offer kept the plain writer - and this is the branch the
         * `keyboard_absent` phase of the storage-boot proof waits for, so a
         * timer record landing inside the line made the marker unfindable
         * (reproduced twice in the ITEM-449 acceptance phase, 2026-09-18).
         * The two lines go through the same atomic writer as the idle offer's.
         */
        length = console_offer_record_append(
            record, sizeof(record), length,
            "[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6 keys=");
        length = console_offer_record_append(record, sizeof(record), length,
                                             keys);
        (void)console_offer_record_append(record, sizeof(record), length, "\n");
        console_service_line(record, strlen(record));
        console_write_atomic(
            console_local_menu_text("console.menu.no_local_input"));
        menu_offer_open = true;
        return;
    }
    length = console_offer_record_append(
        record, sizeof(record), length,
        "[LOCAL-MENU] offered reason=IDLE entries=6 keys=");
    length = console_offer_record_append(record, sizeof(record), length, keys);
    (void)console_offer_record_append(record, sizeof(record), length, "\n");
    console_service_line(record, strlen(record));
    console_write_atomic(
        console_local_menu_text("console.menu.offer_idle"));
    menu_offer_open = true;
}

static void cmd_appliance_boot(void) {
    struct appliance_boot_console_snapshot snapshot;
    char output[APPLIANCE_BOOT_CONSOLE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;
    const enum appliance_boot_console_status snapshot_status =
        appliance_boot_console_snapshot_init(
            &snapshot,
            appliance_identity_get(),
            APPLIANCE_CONSOLE_STATE_OK,
            timer_ticks(),
            boot_cpu_cores,
            usable_memory_mib * 1024U * 1024U);

    if (snapshot_status != APPLIANCE_BOOT_CONSOLE_OK) {
        console_write("[APPLIANCE-BOOT] SNAPSHOT FAILED\n");
        return;
    }
    if (boot_persistence_published &&
        appliance_boot_console_publish_storage(
            &snapshot, boot_storage_ready, boot_backend_count) !=
            APPLIANCE_BOOT_CONSOLE_OK) {
        console_write("[APPLIANCE-BOOT] STORAGE FAILED\n");
        return;
    }
    if (appliance_boot_console_render(
            &snapshot, console_active_language(), output, sizeof(output),
            &output_length) == APPLIANCE_BOOT_CONSOLE_OK) {
        console_write(output);
    } else {
        console_write("[APPLIANCE-BOOT] RENDER FAILED\n");
    }
}

/*
 * ITEM-518: the `storage` command - what the probe saw, with or without a state
 * volume.
 *
 * `appliance-boot` publishes storage only when a persistent volume was
 * published, so on a machine booted from the installation medium - the machine
 * whose installer just refused `NO_DISK` - it says nothing about storage at
 * all.  This command answers from the probe's own report instead: the
 * controllers the class scan matched, the disks they brought up, and the reason
 * each unusable disk carries.  The report is a snapshot the boot published, so
 * the command renders without touching a device and works on a machine where
 * the probe found nothing.
 */
static struct appliance_storage_report console_storage_report;
static bool console_storage_report_published;

void console_publish_storage_report(const struct appliance_storage_report *report)
{
    if (report == NULL) {
        return;
    }
    console_storage_report = *report;
    console_storage_report_published = true;
}

static void cmd_storage(void) {
    static char lines[APPLIANCE_STORAGE_REPORT_LINES_MAX]
                     [APPLIANCE_STORAGE_REPORT_LINE_BYTES];
    struct appliance_storage_report report;
    size_t count;
    size_t index;

    if (console_storage_report_published) {
        report = console_storage_report;
    } else {
        appliance_storage_report_init(&report);
    }
    count = appliance_storage_report_lines(&report, console_active_language(),
                                           lines);
    for (index = 0U; index < count; ++index) {
        console_write(lines[index]);
        console_putc('\n');
    }
}

/*
 * ITEM-519 increment 3: `storage write-proof` drives the write half of the
 * bound USB mass-storage device on the medium itself.  It is a diagnostic, not
 * a mutation of anything the operator owns: the proof reads the last sector,
 * writes a pattern over it, reads it back, restores the sector and reads the
 * restored bytes back, and the serial record `[USB-STORAGE] state=WRITE_PROOF`
 * names every verdict.  A refused or absent device is answered with the same
 * record (`reason=NO_DEVICE` or `reason=BAD_GEOMETRY`) rather than silence.
 */
static void cmd_storage_write_proof(void) {
    if (xhci_storage_write_proof()) {
        console_write("STORAGE WRITE PROOF: OK\n");
    } else {
        console_write("STORAGE WRITE PROOF: FAILED\n");
    }
}

/*
 * ITEM-519 increment 5: `storage medium` asks whether the bound USB device is
 * the medium this machine booted from.  It re-runs the same read-only probe the
 * boot published, so the operator can ask again after a device change, and the
 * answer is the verdict's own name rather than a yes/no that hides why.
 */
static void cmd_storage_medium(void) {
    (void)xhci_storage_medium_publish();
    console_write("STORAGE MEDIUM:");
    console_putc(' ');
    console_write(xhci_storage_medium_result());
    console_putc('\n');
}

#ifdef HFOC_PRODUCTION_INSTALL_MEDIA
/*
 * ITEM-495: the console no longer confirms an install.
 *
 * The console entry used to publish what it prepared and wait for
 * `INSTALL CONFIRM <token>` typed here.  The decision moved to the screen the
 * operator is already looking at - the storage screen shows the disk and warns
 * that everything on it will be deleted - so this command is gone rather than
 * kept as a second, less legible way in.  The unattended entry never came
 * through the console either: it is confirmed by the declaration the medium
 * carries.
 */
#endif

/*
 * ITEM-410: the boot log the console observes records for.  A null binding
 * means no log is bound - the console still paints and the `BOOTLOG` command
 * answers UNAVAILABLE instead of pretending.
 */
static struct console_boot_log_binding boot_log_binding;
static bool boot_log_bound;

void console_bind_boot_log(const struct console_boot_log_binding *binding) {
    if (binding == NULL || binding->observe == NULL ||
        binding->dump == NULL) {
        memset(&boot_log_binding, 0, sizeof(boot_log_binding));
        boot_log_bound = false;
        return;
    }
    boot_log_binding = *binding;
    boot_log_bound = true;
}

/*
 * ITEM-447: the network record binding lives outside the host-test guard
 * because the host proof binds its own two-word owner and drives the whole
 * answer.
 */
void console_bind_network_config(
    const struct console_network_binding *binding) {
    if (binding == NULL || binding->commit == NULL) {
        memset(&network_binding, 0, sizeof(network_binding));
        network_binding_published = false;
        return;
    }
    network_binding = *binding;
    network_binding_published = true;
}

/*
 * ITEM-454: the console's own command names are compared without regard to
 * ASCII case.
 *
 * `help` prints the command names in upper case while the parser compared them
 * byte for byte in lower case, so an operator who typed exactly what help showed
 * was told `UNKNOWN COMMAND` - and the owner, on the acceptance host, could not
 * reach the command the panel's sign-in page named.  The comparison below folds
 * ASCII case only, and only for the name.
 *
 * ITEM-512: the rule is the whole chain's, not the classifier commands' alone.
 * The chain below used raw `strcmp` for menu, clear, version, uptime, ticks,
 * irq, kbd, mem, pmm, heap, about, appliance, bootlog, reboot, shutdown,
 * appliance-boot, panic, divzero, pagefault, doublefault and vmmmap, so a line
 * delivered in upper case - Caps Lock, or the two-controller proof's
 * modifier-only `sendkey shift` wake when the guest's queue drops the release -
 * was refused as `UNKNOWN COMMAND`.  Every command name in the chain now goes
 * through this helper; the argument is never folded (a language value is still
 * exactly `en` or `ru`, and a help topic keeps its own case-insensitive match).
 *
 * Both helpers are compiled into the kernel and into the host proof: the
 * account path itself is production-only (the host build has no enrollment
 * owner), so the rule it depends on is proven where it can be.
 */
static char console_ascii_fold(char value) {
    if (value >= 'A' && value <= 'Z') {
        return (char)(value - 'A' + 'a');
    }
    return value;
}

#ifndef HFOC_CONSOLE_HOST_TEST
/*
 * ITEM-582: the RAM snapshot of a save that had no medium.  The evidence is
 * composed before the medium is judged, so the machine that cannot write still
 * prints the network, panel, entropy and controller records the operator needs
 * to see; `diag snapshot` prints them again after the screen has scrolled.
 * The host build links neither the export nor its snapshot, and the command
 * lives with `diag save` behind the same host guard.
 */
static void print_diag_snapshot(void) {
    const uint32_t records = diag_export_snapshot_records();
    uint32_t index;

    if (records == 0U) {
        return;
    }
    console_write("DIAG SNAPSHOT records=");
    write_u64(records);
    console_putc('\n');
    for (index = 0U; index < records; ++index) {
        const char *line = diag_export_snapshot_line(index);
        uint64_t flags;

        if (line == NULL) {
            break;
        }
        /*
         * ITEM-614: one record is one serial line.  A periodic heartbeat that
         * interleaves inside a composed record cuts the trace fields on the
         * only stream a QEMU proof can read, even though the RAM snapshot is
         * whole.  The production serial publisher already holds a sequence
         * for its record; the snapshot print now does the same.
         */
        flags = serial_sequence_begin();
        console_write(line);
        console_putc('\n');
        serial_sequence_end(flags);
    }
}
#endif /* HFOC_CONSOLE_HOST_TEST */

bool console_command_is(const char *line, const char *command) {
    if (line == NULL || command == NULL) {
        return false;
    }
    while (*line != '\0' && *command != '\0') {
        if (console_ascii_fold(*line) != console_ascii_fold(*command)) {
            return false;
        }
        ++line;
        ++command;
    }
    return *line == '\0' && *command == '\0';
}

bool console_command_prefix(const char *line, const char *prefix) {
    if (line == NULL || prefix == NULL) {
        return false;
    }
    while (*prefix != '\0') {
        if (*line == '\0' ||
            console_ascii_fold(*line) != console_ascii_fold(*prefix)) {
            return false;
        }
        ++line;
        ++prefix;
    }
    return true;
}

#ifdef HFOC_PRODUCTION_TLS
enum console_ssh_key_command_kind {
    CONSOLE_SSH_KEY_COMMAND_NONE = 0,
    CONSOLE_SSH_KEY_COMMAND_SHOW,
    CONSOLE_SSH_KEY_COMMAND_ADD,
    CONSOLE_SSH_KEY_COMMAND_CONFIRM,
    CONSOLE_SSH_KEY_COMMAND_REMOVE,
    CONSOLE_SSH_KEY_COMMAND_USAGE,
};

static struct appliance_ssh_identity *ssh_identity_binding;
static bool (*ssh_revoke_callback)(void *context);
static void *ssh_revoke_context;
static bool ssh_key_pending;
static uint8_t ssh_key_pending_blob[192];
static size_t ssh_key_pending_length;
static uint8_t ssh_key_pending_fingerprint[32];
static char ssh_key_pending_text[64];

void console_bind_ssh_identity(struct appliance_ssh_identity *identity) {
    ssh_identity_binding = identity;
    if (identity == NULL) {
        ssh_key_pending = false;
    }
}

void console_bind_ssh_revoke(bool (*revoke)(void *context), void *context) {
    ssh_revoke_callback = revoke;
    ssh_revoke_context = context;
}

static void console_ssh_key_write(const char *text) {
    console_write(text);
    console_putc('\n');
    console_journal_record(text, strlen(text));
}

static void console_ssh_key_append(char *record, size_t capacity,
                                   size_t *length, const char *text) {
    while (text != NULL && *text != '\0' && *length + 1U < capacity) {
        record[(*length)++] = *text++;
    }
    record[*length] = '\0';
}

static void console_ssh_key_line(const char *first, const char *second,
                                 const char *third) {
    char record[192];
    size_t length = 0U;

    console_ssh_key_append(record, sizeof(record), &length, first);
    console_ssh_key_append(record, sizeof(record), &length, second);
    console_ssh_key_append(record, sizeof(record), &length, third);
    console_ssh_key_write(record);
}

static enum console_ssh_key_command_kind console_ssh_key_classify(
    const char *line, const char **argument, size_t *argument_length) {
    static const char add_prefix[] = "ssh-key add";
    static const char confirm_prefix[] = "ssh-key confirm";

    *argument = NULL;
    *argument_length = 0U;
    if (console_command_is(line, "ssh-key show")) {
        return CONSOLE_SSH_KEY_COMMAND_SHOW;
    }
    if (console_command_is(line, "ssh-key add") ||
        console_command_is(line, "ssh-key confirm") ||
        console_command_is(line, "ssh-key remove") ||
        console_command_is(line, "ssh-key")) {
        return console_command_is(line, "ssh-key remove")
            ? CONSOLE_SSH_KEY_COMMAND_REMOVE
            : CONSOLE_SSH_KEY_COMMAND_USAGE;
    }
    if (console_command_prefix(line, add_prefix) &&
        line[strlen(add_prefix)] == ' ') {
        *argument = line + strlen(add_prefix) + 1U;
        *argument_length = strlen(*argument);
        return CONSOLE_SSH_KEY_COMMAND_ADD;
    }
    if (console_command_prefix(line, confirm_prefix) &&
        line[strlen(confirm_prefix)] == ' ') {
        *argument = line + strlen(confirm_prefix) + 1U;
        *argument_length = strlen(*argument);
        return CONSOLE_SSH_KEY_COMMAND_CONFIRM;
    }
    if (console_command_prefix(line, "ssh-key") &&
        (line[7] == '\0' || line[7] == ' ')) {
        return CONSOLE_SSH_KEY_COMMAND_USAGE;
    }
    return CONSOLE_SSH_KEY_COMMAND_NONE;
}

static void console_ssh_key_show(void) {
    char fingerprint[64];
    char authorized_text[64];

    if (ssh_identity_binding == NULL ||
        !appliance_ssh_identity_host_ready(ssh_identity_binding) ||
        appliance_ssh_identity_fingerprint_text(
            ssh_identity_binding->host_fingerprint, fingerprint,
            sizeof(fingerprint)) == 0U) {
        console_ssh_key_write("[SSH-KEY] host=UNAVAILABLE authorized=NONE");
        return;
    }
    if (!appliance_ssh_identity_authorized_ready(ssh_identity_binding)) {
        console_ssh_key_line("[SSH-KEY] host=", fingerprint,
                             " authorized=NONE");
        return;
    }
    {
        size_t authorized_length = 0U;
        const uint8_t *authorized =
            appliance_ssh_identity_authorized_blob(
                ssh_identity_binding, &authorized_length);
        uint8_t digest[32];

        if (authorized == NULL || authorized_length == 0U ||
            !hfoc_ssh_crypto_public_blob_fingerprint(
                authorized, authorized_length, digest) ||
            appliance_ssh_identity_fingerprint_text(
                digest, authorized_text, sizeof(authorized_text)) == 0U) {
            console_ssh_key_write(
                "[SSH-KEY] host=UNAVAILABLE authorized=NONE");
            return;
        }
    }
    {
        char authorized_line[96];
        size_t authorized_line_length = 0U;

        authorized_line[0] = '\0';
        console_ssh_key_append(authorized_line, sizeof(authorized_line),
                               &authorized_line_length, " authorized=");
        console_ssh_key_append(authorized_line, sizeof(authorized_line),
                               &authorized_line_length, authorized_text);
        console_ssh_key_line("[SSH-KEY] host=", fingerprint,
                             authorized_line);
    }
}

static void console_ssh_key_command(
    enum console_ssh_key_command_kind kind, const char *argument,
    size_t argument_length) {
    if (kind != CONSOLE_SSH_KEY_COMMAND_CONFIRM) {
        ssh_key_pending = false;
    }
    if (kind == CONSOLE_SSH_KEY_COMMAND_SHOW) {
        console_ssh_key_show();
        return;
    }
    if (kind == CONSOLE_SSH_KEY_COMMAND_USAGE) {
        console_ssh_key_write(
            "[SSH-KEY] usage: ssh-key show | ssh-key add <88 base64 point>"
            " | ssh-key confirm <SHA256:...> | ssh-key remove");
        return;
    }
    if (!console_gate_local_secret_proven()) {
        console_ssh_key_write(
            "[SSH-KEY] refused: run `login` or `credentials` first;"
            " enrollment is local-only");
        return;
    }
    if (ssh_identity_binding == NULL ||
        !appliance_ssh_identity_host_ready(ssh_identity_binding)) {
        console_ssh_key_write(
            "[SSH-KEY] refused: the SSH host identity is not available");
        return;
    }
    if (kind == CONSOLE_SSH_KEY_COMMAND_ADD) {
        if (argument_length != 88U ||
            !appliance_ssh_identity_blob_from_point_base64(
                argument, argument_length, ssh_key_pending_blob,
                &ssh_key_pending_length, ssh_key_pending_fingerprint)) {
            ssh_key_pending = false;
            console_ssh_key_write(
                "[SSH-KEY] refused: expected the 88-character base64 of the"
                " P-256 point");
            return;
        }
        if (appliance_ssh_identity_fingerprint_text(
                ssh_key_pending_fingerprint, ssh_key_pending_text,
                sizeof(ssh_key_pending_text)) == 0U) {
            ssh_key_pending = false;
            console_ssh_key_write("[SSH-KEY] refused: fingerprint");
            return;
        }
        ssh_key_pending = true;
        console_ssh_key_line("[SSH-KEY] fingerprint=", ssh_key_pending_text,
                             " type `ssh-key confirm` on the next line");
        return;
    }
    if (kind == CONSOLE_SSH_KEY_COMMAND_CONFIRM) {
        if (!ssh_key_pending || argument_length !=
                                    strlen(ssh_key_pending_text) ||
            memcmp(argument, ssh_key_pending_text, argument_length) != 0) {
            ssh_key_pending = false;
            console_ssh_key_write(
                "[SSH-KEY] refused: no matching pending fingerprint");
            return;
        }
        ssh_key_pending = false;
        /*
         * A replaced key must not leave the old key's live session running.
         * Close sessions first, then write the new authorized record; the
         * endpoint returns to AWAITING_CLIENT_KEY until the next poll.
         */
        if (ssh_revoke_callback != NULL &&
            !ssh_revoke_callback(ssh_revoke_context)) {
            console_ssh_key_write(
                "[SSH-KEY] warning: the previous key's durable revoke"
                " failed; the listener stays stopped and the old key may"
                " return after reboot");
        }
        if (appliance_ssh_identity_authorized_key_add(
                ssh_identity_binding, ssh_key_pending_blob,
                ssh_key_pending_length) != APPLIANCE_SSH_IDENTITY_OK) {
            console_ssh_key_write(
                "[SSH-KEY] refused: the authorized-key record was not"
                " written and read back");
            return;
        }
        console_ssh_key_line("[SSH-KEY] state=ADDED fingerprint=",
                             ssh_key_pending_text, NULL);
        return;
    }
    if (kind == CONSOLE_SSH_KEY_COMMAND_REMOVE) {
        const enum appliance_ssh_identity_status remove_status =
            appliance_ssh_identity_authorized_key_remove(
                ssh_identity_binding);
        /*
         * ITEM-607: the record writer clears the RAM copy before it attempts
         * the durable clear.  A failed write therefore leaves the key revoked
         * for this boot but not provably revoked across a reboot; say exactly
         * that and still close the live listener/session.
         */
        if (ssh_revoke_callback != NULL &&
            !ssh_revoke_callback(ssh_revoke_context)) {
            console_ssh_key_write(
                "[SSH-KEY] warning: the live session revoke failed; the"
                " listener stays stopped");
        }
        ssh_key_pending = false;
        if (remove_status == APPLIANCE_SSH_IDENTITY_STORE_WRITE_FAILED ||
            remove_status == APPLIANCE_SSH_IDENTITY_STORE_READ_FAILED) {
            console_ssh_key_write(
                "[SSH-KEY] state=RAM_REVOKED durable=FAILED; the key may"
                " return after reboot");
            return;
        }
        if (remove_status != APPLIANCE_SSH_IDENTITY_OK) {
            console_ssh_key_write(
                "[SSH-KEY] refused: the authorized-key record was not"
                " cleared");
            return;
        }
        console_ssh_key_write("[SSH-KEY] state=REMOVED authorized=NONE");
        return;
    }
}
#endif /* HFOC_PRODUCTION_TLS */

#ifndef HFOC_CONSOLE_HOST_TEST
/*
 * ITEM-401/ITEM-506: the production account the console conversation calls.
 *
 * The ordinary boot binds the live enrollment owner here; the conversation
 * glue above is compiled into the host proof as well, so the hidden input, the
 * command vocabulary and the refusals are proven without QEMU, and this block
 * is the thin adapter that adds nothing of its own.  The policy, the KDF, the
 * record, the lockout, the migration and the reset all belong to
 * `appliance_production_admin_enrollment`, and every callback below is a
 * one-line forward.
 */
static struct appliance_production_admin_enrollment *admin_enrollment_owner;

static void console_admin_status(void) {
    char record[APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_RENDER_MAX];
    size_t record_length = 0U;

    if (admin_enrollment_owner == NULL ||
        appliance_production_admin_enrollment_render(
            admin_enrollment_owner, record, sizeof(record),
            &record_length) != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        console_write("[ADMIN-ENROLLMENT] state=UNAVAILABLE\n");
        console_journal_record("[ADMIN-ENROLLMENT] state=UNAVAILABLE",
                               strlen("[ADMIN-ENROLLMENT] state=UNAVAILABLE"));
        return;
    }
    console_write(record);
    console_putc('\n');
    /*
     * ITEM-460: the state the operator just read is also the record the durable
     * journal keeps.  The screen line above is the visible answer; this is the
     * structured one a reader of the log finds without a photograph.
     */
    console_journal_record(record, record_length);
}

static enum appliance_production_admin_enrollment_status
console_admin_credentials_verify(
    void *context,
    const char *password,
    size_t password_length,
    uint64_t now_tick) {
    return appliance_production_admin_enrollment_verify_credential(
        context, password, password_length, now_tick);
}

static enum appliance_production_admin_enrollment_status
console_admin_credentials_provision(
    void *context,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick) {
    return appliance_production_admin_enrollment_provision_credential(
        context, login, login_length, password, password_length, now_tick);
}

static enum appliance_production_admin_enrollment_status
console_admin_credentials_authenticate(
    void *context,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out) {
    return appliance_production_admin_enrollment_authenticate_credential(
        context, login, login_length, password, password_length, now_tick,
        lifetime_ticks, token_out, session_id_out);
}

static enum appliance_production_admin_enrollment_status
console_admin_credentials_reset_owner(void *context) {
    return appliance_production_admin_enrollment_reset(context);
}

static const char *console_admin_credentials_status_string(
    enum appliance_production_admin_enrollment_status status) {
    const char *text =
        appliance_production_admin_enrollment_status_string(status);

    return text != NULL ? text : "UNKNOWN";
}

static void console_admin_binding_status(void *context) {
    (void)context;
    console_admin_status();
}

static struct console_admin_credentials_binding
    admin_credentials_production_binding;

void console_bind_admin_enrollment(
    struct appliance_production_admin_enrollment *enrollment) {
    admin_enrollment_owner = enrollment;
    admin_credentials_production_binding.context = enrollment;
    admin_credentials_production_binding.verify =
        console_admin_credentials_verify;
    admin_credentials_production_binding.provision =
        console_admin_credentials_provision;
    admin_credentials_production_binding.authenticate =
        console_admin_credentials_authenticate;
    admin_credentials_production_binding.reset =
        console_admin_credentials_reset_owner;
    admin_credentials_production_binding.status =
        console_admin_binding_status;
    admin_credentials_production_binding.status_text =
        console_admin_credentials_status_string;
    console_bind_admin_credentials(
        enrollment != NULL ? &admin_credentials_production_binding : NULL);
}

void console_bind_admin_credential_rekey(
    bool (*rekey)(
        void *context,
        const char *current_password,
        size_t current_password_length,
        const char *new_password,
        size_t new_password_length))
{
    admin_credentials_production_binding.rekey_credential = rekey;
}
#endif

static void reset_history_state(void) {    history_count = 0U;
    history_oldest = 0U;
    reset_history_navigation_state();
    for (size_t index = 0U; index < HISTORY_CAPACITY; ++index) {
        history[index][0] = '\0';
    }
}

static void console_serial_lock(uint64_t *flags) {
#ifdef HFOC_CONSOLE_HOST_TEST
    (void)flags;
#else
    *flags = cpu_irq_save();
#endif
}

static void console_serial_unlock(uint64_t flags) {
#ifndef HFOC_CONSOLE_HOST_TEST
    cpu_irq_restore(flags);
#else
    (void)flags;
#endif
}

static void reset_history_navigation_state(void) {
    history_navigation_active = false;
    history_navigation_offset = 0U;
    saved_draft_length = 0U;
    saved_draft_cursor = 0U;
    saved_draft[0] = '\0';
}

static size_t history_index_for_offset(size_t offset) {
    return (history_oldest + history_count - 1U - offset) %
           HISTORY_CAPACITY;
}

static void history_add(const char *command) {
    size_t length = strlen(command);
    if (length == 0U) {
        return;
    }
    if (length > INPUT_MAX) {
        length = INPUT_MAX;
    }

    if (history_count != 0U) {
        const size_t newest = (history_oldest + history_count - 1U) %
                              HISTORY_CAPACITY;
        if (strcmp(history[newest], command) == 0) {
            return;
        }
    }

    size_t index;
    if (history_count == HISTORY_CAPACITY) {
        index = history_oldest;
        history_oldest = (history_oldest + 1U) % HISTORY_CAPACITY;
    } else {
        index = (history_oldest + history_count) % HISTORY_CAPACITY;
        ++history_count;
    }
    for (size_t offset = 0U; offset < length; ++offset) {
        history[index][offset] = command[offset];
    }
    history[index][length] = '\0';
}

static void load_history_entry(size_t offset) {
    const char *entry = history[history_index_for_offset(offset)];
    size_t length = 0U;
    while (entry[length] != '\0' && length < INPUT_MAX) {
        input_line[length] = entry[length];
        ++length;
    }
    input_line[length] = '\0';
    input_length = length;
    input_cursor = length;
    redraw_input_line();
}

static void save_draft(void) {
    saved_draft_length = input_length;
    saved_draft_cursor = input_cursor;
    for (size_t index = 0U; index < input_length; ++index) {
        saved_draft[index] = input_line[index];
    }
    saved_draft[input_length] = '\0';
}

static void restore_draft(void) {
    input_length = saved_draft_length;
    input_cursor = saved_draft_cursor;
    for (size_t index = 0U; index < input_length; ++index) {
        input_line[index] = saved_draft[index];
    }
    input_line[input_length] = '\0';
    redraw_input_line();
}

static void reset_input_state(void) {
    input_length = 0U;
    input_cursor = 0U;
    previous_rendered_length = 0U;
    input_line[0] = '\0';
    input_overflow = false;
}


/*
 * ITEM-457: what the console's own scrolling cost.
 *
 * The 2026-09-19 hardware report was "when the output reaches the bottom of
 * the screen, the refresh is very slow", and the keyboard - polled from the
 * same main loop - answered late while it happened.  framebuffer_scroll_up()
 * moved the body one pixel at a time; it moves whole rows now, and the
 * console publishes the measurement instead of leaving the fix to be
 * believed:
 *
 *   [CONSOLE] scroll rows=<total> bytes=<total> us=<average> max_us=<worst>
 *
 * `rows` is how many text-row scrolls this boot has done, `bytes` how much
 * framebuffer those moves wrote, `us` the average cost of the scrolls since
 * the previous report and `max_us` the worst single one so far.  The clock is
 * the processor's cycle counter (ITEM-463): the 100 Hz tick resolves neither
 * the row move nor the per-pixel move it replaced, so the microsecond reading
 * is what makes the two different.  The average over the whole window is the
 * number that answers the report - one scrolled text line used to write about
 * two million pixels.
 *
 * The record is written with the atomic writer, because a proof waits for the
 * whole line and an IRQ record landing inside it would split the marker.
 *
 * Publishing a record scrolls the screen itself, and a record written per
 * scrolled row would pay a serial write per row - the very cost this item is
 * about.  So a report is due when it is the first one, when a scroll was
 * slower than any reported before, or every CONSOLE_SCROLL_REPORT_ROWS rows;
 * the numbers are cumulative, so a reader who sees only the first report still
 * sees the total grow in the next one.  A report caused by a report is
 * suppressed: the nested scroll's cost is counted and the next real scroll
 * publishes it.
 */
#define CONSOLE_SCROLL_RECORD_MAX 128U
#define CONSOLE_SCROLL_REPORT_ROWS 16U

static uint64_t console_scroll_rows;
static uint64_t console_scroll_bytes;
static uint64_t console_scroll_window_rows;
static uint64_t console_scroll_window_us;
static uint64_t console_scroll_max_us;
/*
 * ITEM-463: the cycles the scrolls inside a paint burst spent.  A
 * `console_write()` that ends a line and reaches the bottom both paints its
 * characters and moves the whole body, and the two costs are measured and
 * reported separately: charging the 4K body move to the line paint made a
 * paint budget bound the wrong thing.  `console_write()` and the record
 * mirror snapshot this total and subtract what the burst's own newlines
 * added, so `[CONSOLE] paint` is the cost of the characters it drew.
 */
static uint64_t console_scroll_cycles_total;
static uint64_t console_scroll_reported_rows;
static uint64_t console_scroll_reported_max_us;
static bool console_scroll_publishing;
/*
 * ITEM-620: true while the current console line still has unpainted text.  A
 * scroll record is only published at a line boundary, so it can never be
 * injected in the middle of another record (the diag snapshot and the panel
 * status are written in several `console_write` calls, and a wrap-induced
 * scroll used to split them).
 */
static bool console_line_open;

/*
 * ITEM-474 (round 13): the turn's batched scroll.
 *
 * The round-13 hardware log read a loop that turned 2.4 times a second
 * (`iter_us=424 022`, `loops` 99 -> 195 over 42 real seconds) and a single
 * screen scroll that moved the whole body: `[CONSOLE] scroll rows=1
 * bytes=31979520 us=7198` on a 3840x2160 screen.  A record printed per turn
 * scrolls once, and the record writers hold interrupts off for their whole
 * serial+screen sequence, so every scrolled line is also real time the PIT
 * cannot count.  The fix is the one the contract asks for: a turn paints its
 * lines, and the body moves once for all of them.
 *
 * The console keeps painting at `cursor_y` as if each newline had scrolled.
 * The first newline that would leave the body opens a batch: the logical
 * cursor stays on the bottom row and `console_scroll_pending_cells` counts the
 * rows the turn has overflowed.  Every character after that is serialized at
 * once, as before, but its paint is appended to `console_defer_buffer`
 * instead of the framebuffer.  `console_flush_deferred()` then performs one
 * `framebuffer_scroll_up()` for the whole batch and replays the buffered
 * characters through the ordinary painter (with scrolling disabled), starting
 * at the row the first buffered line belongs on.  The screen therefore ends
 * exactly as if each line had scrolled, with one body move instead of one per
 * line.  `console_begin_turn_batching()` opens the per-turn window in the main
 * loop; before the loop, and in the host proofs, `console_write()` flushes each
 * burst on its own so the boot screen is never left behind a batch.
 */
#define CONSOLE_DEFER_MAX 4096U

static char console_defer_buffer[CONSOLE_DEFER_MAX];
static size_t console_defer_length;
static uint64_t console_scroll_pending_cells;
static bool console_deferring;
static bool console_replaying;
static bool console_defer_batching;
static bool console_flush_active;
static unsigned int console_write_depth;
static uint64_t console_cost_us_total;
/*
 * ITEM-482 (round 20): the worst wait between a USB keyboard report being
 * decoded and the character it carried reaching the screen.  The transport
 * counters say the reports arrive; this is the number that says whether the
 * operator's "the keyboard lags" is the echo, and it is only meaningful for
 * the USB path, whose events carry their completion clock.
 */
static uint64_t console_kbd_echo_us_max;

/*
 * MOUSE-DIAG (owner task 2026-09-27): the keyboard echo's own split.
 *
 * `kbd_draw_us` says how long the echo took but not what it did: one painted
 * cell (the ITEM-482 fast path) or a whole line redrawn because the text moved
 * (mid-line edit, LEFT/RIGHT/HOME/END, DELETE, history) or because another
 * writer had moved the screen since the line was rendered (ITEM-483's
 * generation check).  Without that split a slow keystroke cannot be attributed,
 * and a fast path that is silently never taken looks like a slow console.  The
 * counters describe the echo; they do not decide it.
 */
static uint64_t console_kbd_echo_fast_count;
static uint64_t console_kbd_echo_redraw_count;
static uint64_t console_kbd_echo_redraw_chars;
static uint64_t console_kbd_echo_redraw_us_total;
static uint64_t console_kbd_echo_redraw_us_max;
static uint64_t console_kbd_echo_generation_invalidations;

static void console_emit(char value, bool to_serial);
void console_flush_deferred(void);

/*
 * ITEM-482 (round 20): one write-combining drain.  The keyboard's share is
 * measured only while its echo is being painted, and the measurement's own
 * timer reads stay out of every other caller's cost - the scroll and record
 * records time themselves and must not be charged two extra reads.
 */
static void console_sync(void) {
    if (!console_keyboard_echo_active) {
        framebuffer_sync();
        return;
    }
    {
        const uint64_t started = timer_cycles();

        framebuffer_sync();
        console_keyboard_sync_us_total +=
            timer_cycles_to_us(timer_cycles() - started);
    }
}

uint64_t console_keyboard_sync_us(void) {
    return console_keyboard_sync_us_total;
}

/*
 * ITEM-482 (round 20): close the turn's input batch with a single drain.
 *
 * The keyboard's fast path, the mouse's up-to-three coalesced batches and the
 * button edges all run first; their writes queue in write-combining memory and
 * one `framebuffer_sync()` makes them visible.  Doing it here rather than once
 * per batch is the model the item fixes: many events, one drain.
 */
void console_end_input_batch(void) {
    if (!console_input_needs_sync) {
        return;
    }
    console_input_needs_sync = false;
    {
        const uint64_t started = timer_cycles();

        framebuffer_sync();
        console_pointer_sync_us_total +=
            timer_cycles_to_us(timer_cycles() - started);
    }
}

/* ITEM-474: the whole cost the console charged to paint and scroll this boot,
   in microseconds, for the loop's own phase record. */
uint64_t console_cost_us(void) {
    return console_cost_us_total;
}

/* ITEM-482: the keyboard's echo latency, published by the input window record. */
uint64_t console_kbd_echo_us_max_value(void) {
    return console_kbd_echo_us_max;
}

/* MOUSE-DIAG (2026-09-27): the echo split the keyboard contract asked for. */
uint64_t console_input_echo_fast_count(void) {
    return console_kbd_echo_fast_count;
}

uint64_t console_input_echo_redraw_count(void) {
    return console_kbd_echo_redraw_count;
}

uint64_t console_input_echo_redraw_chars(void) {
    return console_kbd_echo_redraw_chars;
}

uint64_t console_input_echo_redraw_us_total(void) {
    return console_kbd_echo_redraw_us_total;
}

uint64_t console_input_echo_redraw_us_max(void) {
    return console_kbd_echo_redraw_us_max;
}

uint64_t console_input_echo_generation_invalidations(void) {
    return console_kbd_echo_generation_invalidations;
}

/* ITEM-474: open the per-turn batch after the boot has finished painting. */
void console_begin_turn_batching(void) {
    console_defer_batching = true;
}

static bool console_defer_push(char value) {
    if (!console_deferring || console_replaying || console_flush_active) {
        return false;
    }
    if (console_defer_length >= CONSOLE_DEFER_MAX) {
        console_flush_deferred();
        return false;
    }
    console_defer_buffer[console_defer_length++] = value;
    return true;
}

/*
 * ITEM-509 increment 3: the deferred batch keeps whole code points.
 *
 * A multi-byte sequence must land in the batch as one unit: the replay at the
 * flush decodes what it finds there, and a sequence cut in half by the 4 KiB
 * bound would be replayed as the marker plus a stray byte - a different screen
 * from the one the live paint produced.  A sequence that does not fit flushes
 * the batch first, exactly as a single byte at the bound does, and the caller
 * paints the code point directly.
 */
static bool console_defer_push_bytes(const char *bytes, size_t length) {
    if (!console_deferring || console_replaying || console_flush_active) {
        return false;
    }
    if (length == 0U || length > CONSOLE_DEFER_MAX) {
        return false;
    }
    if (console_defer_length + length > CONSOLE_DEFER_MAX) {
        console_flush_deferred();
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        console_defer_buffer[console_defer_length++] = bytes[index];
    }
    return true;
}

static size_t console_scroll_append_u64(char *output, size_t capacity,
                                        size_t length, uint64_t value) {
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U && length + 1U < capacity) {
        output[length++] = digits[--count];
    }
    if (capacity != 0U) {
        output[length] = '\0';
    }
    return length;
}

/* ITEM-472: a signed applied delta, so a window that moved left reads negative
   instead of being confused with a large unsigned value. */
static size_t console_pointer_append_signed(char *output, size_t capacity,
                                            size_t length, int64_t value) {
    uint64_t magnitude;

    if (value < 0) {
        if (length + 1U < capacity) {
            output[length++] = '-';
        }
        magnitude = (uint64_t)(-(value + 1)) + 1U;
    } else {
        magnitude = (uint64_t)value;
    }
    return console_scroll_append_u64(output, capacity, length, magnitude);
}

/* The average microseconds one scroll of the window took, rounded down. */
static uint64_t console_scroll_average_us(void) {
    if (console_scroll_window_rows == 0U) {
        return 0U;
    }
    return console_scroll_window_us / console_scroll_window_rows;
}

static bool console_scroll_report_due(void) {
    if (console_scroll_rows == 0U) {
        return false;
    }
    if (console_scroll_reported_rows == 0U) {
        return true;
    }
    /*
     * ITEM-463: the worst is cumulative, so the sixteen-row cadence already
     * publishes it.  The "worse than reported" trigger now needs a meaningful
     * margin: the microsecond clock makes every noisy scroll a new maximum,
     * and reporting each one flooded the serial stream and split the console
     * echoes other proofs read (the 2026-09-20 `test-production-storage-boot`
     * failure).  A scroll half again as slow as the reported worst is still
     * published at once.
     */
    if (console_scroll_max_us >
        console_scroll_reported_max_us +
            console_scroll_reported_max_us / 2U + 20000U) {
        return true;
    }
    return console_scroll_rows - console_scroll_reported_rows >=
           CONSOLE_SCROLL_REPORT_ROWS;
}

static void console_publish_scroll(void) {
    char scroll_record[CONSOLE_SCROLL_RECORD_MAX];
    const uint64_t published_rows = console_scroll_rows;
    const uint64_t published_max = console_scroll_max_us;
    size_t length = 0U;

    /*
     * ITEM-460: a geometry record is being written, and its own newline is the
     * scroll that makes this report due.  Publishing it now would add a second
     * line below the geometry the record just named, move the menu up one more
     * row, and leave the pointer aimed at the entry under the one it wants -
     * the 2026-09-20 `test-usb-mouse-menu-qemu` run hit exactly that.  The
     * report is not lost: the write that follows the geometry publishes it,
     * after `pointer_publishing` has cleared, and that writer republishes the
     * corrected geometry like any other record.
     */
    if (console_scroll_publishing || pointer_publishing ||
        console_line_open || !console_scroll_report_due()) {
        return;
    }
    console_scroll_publishing = true;
    length = console_offer_record_append(scroll_record, sizeof(scroll_record),
                                         length, "[CONSOLE] scroll rows=");
    length = console_scroll_append_u64(scroll_record, sizeof(scroll_record),
                                       length, published_rows);
    length = console_offer_record_append(scroll_record, sizeof(scroll_record),
                                         length, " bytes=");
    length = console_scroll_append_u64(scroll_record, sizeof(scroll_record),
                                       length, console_scroll_bytes);
    length = console_offer_record_append(scroll_record, sizeof(scroll_record),
                                         length, " us=");
    length = console_scroll_append_u64(scroll_record, sizeof(scroll_record),
                                       length, console_scroll_average_us());
    length = console_offer_record_append(scroll_record, sizeof(scroll_record),
                                         length, " max_us=");
    length = console_scroll_append_u64(scroll_record, sizeof(scroll_record),
                                       length, published_max);
    length = console_offer_record_append(scroll_record, sizeof(scroll_record),
                                         length, "\n");
    if (length + 1U < sizeof(scroll_record)) {
        console_write_atomic(scroll_record);
        /*
         * ITEM-461: the scroll cost also reaches the durable journal, so a run
         * read from the disk carries the number the owner asked for instead of
         * only the screen a photograph would have to catch.
         */
        console_publish_journal(scroll_record, length);
        console_scroll_reported_rows = published_rows;
        console_scroll_reported_max_us = published_max;
        console_scroll_window_rows = 0U;
        console_scroll_window_us = 0U;
    }
    console_scroll_publishing = false;
}

/*
 * ITEM-461: what a contiguous paint burst costs.
 *
 * The scroll report says what moving the screen costs; this says what putting a
 * line on it costs, the other half of the owner's 2026-09-20 "the redraw is
 * slow".  The measurement is deliberately the cost of a burst the console
 * itself writes in one go - one `console_write()` that ends a line, or one
 * mirrored record - because the interactive line is assembled keystroke by
 * keystroke and charging the operator's own typing time to the paint would make
 * the number a lie.  The clock is the same 100 Hz tick, so the reading is the
 * average over a window plus the worst burst, and the record is journal-only: a
 * cost record that painted itself would pay the cost it exists to publish.  The
 * cadence mirrors the scroll report's, so a report cannot become the flood.
 */
#define CONSOLE_PAINT_RECORD_MAX 128U
#define CONSOLE_PAINT_REPORT_LINES 64U

static uint64_t console_paint_lines;
static uint64_t console_paint_chars;
static uint64_t console_paint_window_lines;
static uint64_t console_paint_window_us;
static uint64_t console_paint_max_us;
static uint64_t console_paint_reported_lines;
static uint64_t console_paint_reported_max_us;
static bool console_paint_publishing;

static uint64_t console_paint_average_us(void) {
    if (console_paint_window_lines == 0U) {
        return 0U;
    }
    return console_paint_window_us / console_paint_window_lines;
}

static bool console_paint_report_due(void) {
    if (console_paint_lines == 0U) {
        return false;
    }
    if (console_paint_reported_lines == 0U ||
        console_paint_max_us > console_paint_reported_max_us) {
        return true;
    }
    return console_paint_lines - console_paint_reported_lines >=
           CONSOLE_PAINT_REPORT_LINES;
}

static void console_publish_paint(void) {
    char paint_record[CONSOLE_PAINT_RECORD_MAX];
    const uint64_t published_lines = console_paint_lines;
    const uint64_t published_max = console_paint_max_us;
    size_t length = 0U;

    if (console_paint_publishing || !console_paint_report_due()) {
        return;
    }
    console_paint_publishing = true;
    length = console_offer_record_append(
        paint_record, sizeof(paint_record), length, "[CONSOLE] paint lines=");
    length = console_scroll_append_u64(paint_record, sizeof(paint_record),
                                       length, published_lines);
    length = console_offer_record_append(paint_record, sizeof(paint_record),
                                         length, " chars=");
    length = console_scroll_append_u64(paint_record, sizeof(paint_record),
                                       length, console_paint_chars);
    length = console_offer_record_append(paint_record, sizeof(paint_record),
                                         length, " us=");
    length = console_scroll_append_u64(paint_record, sizeof(paint_record),
                                       length, console_paint_average_us());
    length = console_offer_record_append(paint_record, sizeof(paint_record),
                                         length, " max_us=");
    length = console_scroll_append_u64(paint_record, sizeof(paint_record),
                                       length, published_max);
    if (length + 1U < sizeof(paint_record)) {
        console_publish_journal(paint_record, length);
        console_paint_reported_lines = published_lines;
        console_paint_reported_max_us = published_max;
        console_paint_window_lines = 0U;
        console_paint_window_us = 0U;
    }
    console_paint_publishing = false;
}

static void console_paint_record_lines(uint64_t lines, uint64_t cycles) {
    const uint64_t micros = timer_cycles_to_us(cycles);

    if (lines == 0U) {
        return;
    }
    console_paint_lines += lines;
    console_paint_window_lines += lines;
    console_paint_window_us += micros;
    if (micros > console_paint_max_us) {
        console_paint_max_us = micros;
    }
    console_publish_paint();
}

/*
 * ITEM-474: one batched body move for every row the turn overflowed.
 *
 * This is the whole cost that used to be paid once per scrolled line: the
 * pixels, the scroll counters and the pointer geometry all move by `cells`
 * rows in a single `framebuffer_scroll_up()`.  The caller replays the buffered
 * characters into the rows this move frees.
 */
static void console_scroll_apply(uint64_t cells) {
    uint64_t started;
    uint64_t moved;
    uint64_t elapsed_cycles;
    uint64_t elapsed_us;

    if (cells == 0U) {
        return;
    }
    started = timer_cycles();
    moved = framebuffer_scroll_up(BODY_TOP, cells * console_cell_height,
                                  BACKGROUND);
    /* ITEM-464: the row move is write-combining traffic; drain it before the
       cost is read so `scroll us=` is the move, not the queueing. */
    console_sync();
    elapsed_cycles = timer_cycles() - started;
    elapsed_us = timer_cycles_to_us(elapsed_cycles);

    console_scroll_cycles_total += elapsed_cycles;
    console_cost_us_total += elapsed_us;
    console_scroll_rows += cells;
    console_scroll_window_rows += cells;
    console_scroll_window_us += elapsed_us;
    console_scroll_bytes += moved;
    if (elapsed_us > console_scroll_max_us) {
        console_scroll_max_us = elapsed_us;
    }
}

/*
 * ITEM-474: how many text rows the body holds.  A batch never grows past this
 * many rows: the replay starts at `height - cell - (cells - 1) * cell`, so a
 * larger batch would place the first line above `BODY_TOP` and underflow the
 * cursor.  A turn that prints more than a screenful therefore flushes in whole
 * screenfuls, each still one body move.
 */
static uint64_t console_scroll_batch_limit(void) {
    const uint64_t body = framebuffer_height() > BODY_TOP
                              ? framebuffer_height() - BODY_TOP
                              : 0U;
    uint64_t rows =
        console_cell_height != 0U ? body / console_cell_height : 0U;

    if (rows == 0U) {
        rows = 1U;
    }
    return rows;
}

/*
 * ITEM-474: the deferred newline.  A line that would leave the body does not
 * move it; it opens (or extends) the batch and keeps the logical cursor on the
 * last row, exactly where the single move will leave it.  Every character the
 * turn prints after that is replayed by `console_flush_deferred()`.  When the
 * batch reaches a full screenful it flushes at once, so the replay always
 * starts on a row inside the body.  It answers whether that cap flush ran: the
 * newline that caused it was already applied by the flush and must not also be
 * replayed from the fresh batch.
 */
static bool new_line(void) {
    const uint64_t previous_y = cursor_y;
    bool flushed = false;

    cursor_y += console_cell_height;
    if (cursor_y + console_cell_height > framebuffer_height()) {
        if (console_replaying) {
            /* The batch counted every row the replay can overflow, so this
               should not happen; keep the cursor on the last row if it does. */
            cursor_y = previous_y;
            cursor_x = LEFT;
            return false;
        }
        ++console_scroll_pending_cells;
        console_deferring = true;
        cursor_y = previous_y;
        if (console_scroll_pending_cells >= console_scroll_batch_limit()) {
            console_flush_deferred();
            flushed = true;
        }
    }
    /*
     * ITEM-474: the new row starts at the left edge.  The cap flush above
     * replays the batch and leaves the cursor at the last replayed line, so a
     * wrapping character painted after it would otherwise land at the end of
     * that line instead of on its own row.
     */
    cursor_x = LEFT;
    return flushed;
}

/*
 * ITEM-474: apply the batch, then replay what it deferred.
 *
 * The buffered characters are replayed through the ordinary painter with
 * scrolling disabled: the single body move has already made the rows they
 * belong on, starting at the row the first buffered line would have occupied
 * had each newline scrolled on its own.  The buffer is read in place while
 * `console_replaying` is set, so a deferred character can never be buffered
 * again.  A nested flush (the pointer geometry or the scroll report writing on
 * its own) is refused rather than re-entered; its output is buffered and the
 * next flush handles it.
 */
void console_flush_deferred(void) {
    uint64_t cells;
    size_t replay_length;
    uint64_t replay_started = 0U;

    if (!console_deferring || console_replaying || console_flush_active) {
        return;
    }
    console_flush_active = true;
    cells = console_scroll_pending_cells;
    replay_length = console_defer_length;
    console_deferring = false;
    console_scroll_pending_cells = 0U;
    console_defer_length = 0U;
    if (cells != 0U) {
        /*
         * ITEM-482 (round 20): hide the pointer *before* the body moves.  The
         * cursor sprite lives in the same surface the row copy moves, so a
         * visible pointer would be carried up with the text as a ghost and the
         * saved block would restore stale pixels at the old place.  Hiding it
         * here puts the surface back, then the move is clean, and the pointer
         * is drawn again at its own position after the replay.
         */
        pointer_suspend();
        console_scroll_apply(cells);
    }
    if (replay_length != 0U) {
        cursor_x = LEFT;
        if (cells != 0U) {
            cursor_y = framebuffer_height() - console_cell_height -
                       (cells - 1U) * console_cell_height;
        } else {
            cursor_y = framebuffer_height() - console_cell_height;
        }
        pointer_suspend();
        console_replaying = true;
        replay_started = timer_cycles();
        /*
         * ITEM-509 increment 3: the batch holds whole code points, so the
         * replay decodes them exactly as the live paint did.  A byte that does
         * not decode is the one the live path refused and is replayed as the
         * same marker, and the `[GLYPH-MISSING]` record is published here for
         * the cells this replay is the first to draw - a code point painted
         * live published its own record at that paint, so no cell is counted
         * twice.
         */
        for (size_t index = 0U; index < replay_length; ) {
            const unsigned char byte =
                (unsigned char)console_defer_buffer[index];

            if (byte < 0x80U) {
                console_emit((char)byte, false);
                ++index;
                continue;
            }
            {
                uint32_t codepoint = 0U;
                size_t next = index;

                if (appliance_language_utf8_next(console_defer_buffer,
                                                 replay_length, &next,
                                                 &codepoint)) {
                    console_emit_codepoint(codepoint,
                                           console_defer_buffer + index,
                                           next - index, false, -1);
                    index = next;
                } else {
                    console_emit_codepoint(0xFFFDU,
                                           console_defer_buffer + index,
                                           1U, false, (int)byte);
                    ++index;
                }
            }
        }
        console_cost_us_total +=
            timer_cycles_to_us(timer_cycles() - replay_started);
        console_replaying = false;
        pointer_resume();
    } else if (cells != 0U) {
        pointer_resume();
    }
    /*
     * ITEM-474: the pointer geometry and the scroll report are published with
     * the flush guard released - their own writes may scroll the body, and the
     * geometry's own scroll must be applied while `pointer_publishing` still
     * suppresses the recursive publish.  The writes are rare; the turn's flood
     * was already replayed above.
     */
    console_flush_active = false;
    if (cells != 0U) {
        /* ITEM-448: every painted line moved up together, so the rectangles a
           click was hit-testing move with them - and the ones that left the
           body are dropped instead of staying behind at a row the screen no
           longer shows them on. */
        pointer_scroll_targets(cells);
        /* ITEM-457: the report comes last, after the pointer geometry it may
           displace has been published. */
        console_publish_scroll();
    }
}

static void erase_cell(void) {
    framebuffer_rect(cursor_x, cursor_y, console_cell_width, console_cell_height, BACKGROUND);
}

/*
 * ITEM-484 (round 22): an echo cell is a write-combining store the input
 * batch's single drain owes.
 *
 * The fast path's appended cell and the deferred replay's cells are painted
 * while `console_keyboard_echo_active`/`console_replaying` is set; marking the
 * batch here means the one `console_end_input_batch()` that closes the keyboard
 * plus the bounded mouse batches also makes the echo visible, so the operator's
 * character no longer waits for the turn's tail.  Draw telemetry stays where it
 * was: this only raises the drain flag.
 */
static void console_note_echo_paint(void) {
    if (console_keyboard_echo_active || console_replaying) {
        console_input_needs_sync = true;
    }
}

/*
 * ITEM-405: one character painter, two surfaces.
 *
 * `to_serial` selects whether the character also reaches the serial stream.
 * The interactive console and every pre-existing caller keep writing both
 * surfaces through `console_putc()`; the ITEM-405 record mirror paints the
 * framebuffer only, because the record it mirrors has already been written to
 * serial by its owner and a duplicate there would change a stream the contract
 * modules and the proofs pin.
 */
/*
 * ITEM-618: a bounded text scrollback for the local console.
 *
 * The framebuffer keeps only the cells that fit the screen, so a long
 * `network`/`diag` answer disappears above the top row.  This ring keeps the
 * body text the console already painted - one logical line per slot - and the
 * `scroll` command redraws any page of it without blocking the production
 * loop.  The ring is captured outside the input echo and outside the pager's
 * own repaint, so a password star, a typed secret or a redraw never becomes a
 * second history entry.
 */
#define SCROLLBACK_LINES 128U
#define SCROLLBACK_LINE_MAX 1024U

static char scrollback_lines[SCROLLBACK_LINES][SCROLLBACK_LINE_MAX];
static uint16_t scrollback_lengths[SCROLLBACK_LINES];
static uint32_t scrollback_next;
static uint32_t scrollback_count;
static char scrollback_current[SCROLLBACK_LINE_MAX];
static uint16_t scrollback_current_length;
static bool scrollback_muted;

static bool pager_active;
static uint32_t pager_top;

static uint32_t scrollback_total_lines(void) {
    uint32_t total = scrollback_count;

    if (scrollback_current_length != 0U) {
        ++total;
    }
    return total;
}

static void scrollback_current_append(unsigned char value) {
    if (scrollback_current_length + 1U >= SCROLLBACK_LINE_MAX) {
        return;
    }
    scrollback_current[scrollback_current_length++] = (char)value;
    scrollback_current[scrollback_current_length] = '\0';
}

static void scrollback_capture_byte(unsigned char value) {
    if (scrollback_muted || console_replaying) {
        return;
    }
    /* A newline is a line boundary even while the keystroke echo is active;
       the typed characters themselves are never captured. */
    if (value != '\n' && console_keyboard_echo_active) {
        return;
    }
    if (value == '\n') {
        const uint16_t length = scrollback_current_length;

        memcpy(scrollback_lines[scrollback_next], scrollback_current,
               (size_t)length + 1U);
        scrollback_lengths[scrollback_next] = length;
        scrollback_next = (scrollback_next + 1U) % SCROLLBACK_LINES;
        if (scrollback_count < SCROLLBACK_LINES) {
            ++scrollback_count;
        }
        scrollback_current_length = 0U;
        scrollback_current[0] = '\0';
        return;
    }
    if (value == '\r') {
        return;
    }
    if (value == '\b') {
        if (scrollback_current_length != 0U) {
            uint16_t length = scrollback_current_length;

            while (length != 0U &&
                   ((unsigned char)scrollback_current[length - 1U] &
                    0xC0U) == 0x80U) {
                --length;
            }
            if (length != 0U) {
                --length;
            }
            scrollback_current_length = length;
            scrollback_current[length] = '\0';
        }
        return;
    }
    if (value == '\t') {
        for (unsigned int index = 0U; index < 4U; ++index) {
            scrollback_current_append(' ');
        }
        return;
    }
    scrollback_current_append(value);
}

static const char *scrollback_line_at(uint32_t offset) {
    const uint32_t first =
        (scrollback_next + SCROLLBACK_LINES - scrollback_count) %
        SCROLLBACK_LINES;

    if (offset < scrollback_count) {
        return scrollback_lines[(first + offset) % SCROLLBACK_LINES];
    }
    if (offset == scrollback_count && scrollback_current_length != 0U) {
        return scrollback_current;
    }
    return NULL;
}

static uint32_t pager_rows(void) {
    const uint64_t body =
        framebuffer_height() > BODY_TOP + console_cell_height + 4U
            ? framebuffer_height() - BODY_TOP - console_cell_height - 4U
            : console_cell_height;

    return (uint32_t)(body / console_cell_height);
}

static size_t pager_columns(void) {
    const uint64_t width =
        framebuffer_width() > LEFT + console_cell_width
            ? framebuffer_width() - LEFT
            : console_cell_width;

    return (size_t)(width / console_cell_width);
}

/*
 * The byte length of the first `max_codepoints` code points of a stored line,
 * and the number of code points it holds.  A malformed byte counts as one
 * cell; the ring only holds bytes the console already decoded and painted.
 */
static size_t scrollback_prefix_bytes(const char *text, size_t max_codepoints,
                                      size_t *codepoints_out) {
    size_t bytes = 0U;
    size_t codepoints = 0U;

    while (text[bytes] != '\0' && codepoints < max_codepoints) {
        const unsigned char lead = (unsigned char)text[bytes];
        size_t length = 1U;

        if ((lead & 0xE0U) == 0xC0U) {
            length = 2U;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3U;
        } else if ((lead & 0xF8U) == 0xF0U) {
            length = 4U;
        }
        bytes += length;
        ++codepoints;
    }
    if (codepoints_out != NULL) {
        *codepoints_out = codepoints;
    }
    return bytes;
}

static size_t scrollback_line_codepoints(const char *text) {
    size_t codepoints = 0U;

    (void)scrollback_prefix_bytes(text, SCROLLBACK_LINE_MAX, &codepoints);
    return codepoints;
}

static uint32_t pager_visual_rows(void) {
    const uint32_t total = scrollback_total_lines();
    const size_t columns = pager_columns();
    uint32_t rows = 0U;

    for (uint32_t offset = 0U; offset < total; ++offset) {
        const char *line = scrollback_line_at(offset);
        const size_t codepoints =
            line != NULL ? scrollback_line_codepoints(line) : 0U;
        const size_t line_rows =
            codepoints > columns ? (codepoints + columns - 1U) / columns : 1U;

        rows += (uint32_t)line_rows;
    }
    return rows;
}

static void pager_paint(void) {
    const uint32_t total = scrollback_total_lines();
    const uint32_t rows = pager_rows();
    const uint32_t visual_rows = pager_visual_rows();
    const size_t columns = pager_columns();
    const uint64_t footer_y = framebuffer_height() - console_cell_height - 2U;
    uint32_t skip;
    uint32_t drawn = 0U;

    if (pager_top + rows > visual_rows) {
        pager_top = visual_rows > rows ? visual_rows - rows : 0U;
    }
    skip = pager_top;
    scrollback_muted = true;
    pointer_suspend();
    framebuffer_rect(0, BODY_TOP, framebuffer_width(),
                     framebuffer_height() - BODY_TOP, BACKGROUND);
    cursor_x = LEFT;
    cursor_y = BODY_TOP + 8U;
    if (total == 0U) {
        console_write(console_local_menu_text("console.prose.scroll_empty"));
    } else {
        for (uint32_t offset = 0U; offset < total && drawn < rows; ++offset) {
            const char *line = scrollback_line_at(offset);
            const size_t codepoints =
                line != NULL ? scrollback_line_codepoints(line) : 0U;
            size_t start = 0U;

            do {
                char chunk[SCROLLBACK_LINE_MAX];
                size_t chunk_codepoints = 0U;
                size_t length = 0U;

                if (line != NULL) {
                    length = scrollback_prefix_bytes(line + start, columns,
                                                     &chunk_codepoints);
                }
                if (skip != 0U) {
                    --skip;
                } else if (drawn < rows) {
                    if (length != 0U) {
                        memcpy(chunk, line + start, length);
                        chunk[length] = '\0';
                        console_write(chunk);
                    }
                    console_putc('\n');
                    ++drawn;
                }
                start += length;
            } while (line != NULL && start < codepoints && drawn < rows);
        }
    }
    cursor_x = LEFT;
    cursor_y = footer_y;
    console_write(console_local_menu_text("console.prose.scroll_hint"));
    pointer_resume();
    scrollback_muted = false;
}

static void pager_leave(void) {
    pager_active = false;
    clear_body();
    prompt();
    reset_history_navigation_state();
    reset_input_state();
}

static void pager_handle_event(const struct keyboard_event *event) {
    const uint32_t total = pager_visual_rows();
    const uint32_t rows = pager_rows();

    switch (event->type) {
    case KEYBOARD_EVENT_UP:
        if (pager_top != 0U) {
            --pager_top;
            pager_paint();
        }
        return;
    case KEYBOARD_EVENT_DOWN:
        if (pager_top + rows < total) {
            ++pager_top;
            pager_paint();
        }
        return;
    case KEYBOARD_EVENT_HOME:
        pager_top = 0U;
        pager_paint();
        return;
    case KEYBOARD_EVENT_END:
        pager_top = total > rows ? total - rows : 0U;
        pager_paint();
        return;
    case KEYBOARD_EVENT_CHARACTER:
        if (event->character == 'q' || event->character == 'Q') {
            pager_leave();
        }
        return;
    case KEYBOARD_EVENT_ENTER:
        pager_leave();
        return;
    default:
        return;
    }
}

#define CONSOLE_WHEEL_ROWS 3U

static uint32_t pager_bottom_top(void) {
    const uint32_t total = pager_visual_rows();
    const uint32_t rows = pager_rows();

    return total > rows ? total - rows : 0U;
}

/*
 * ITEM-621: one wheel gesture moves the retained viewport by a bounded number
 * of text rows.  Wheel up (positive, away from the user) enters the history at
 * its newest page and walks towards the oldest line; wheel down walks back and
 * leaves the viewer for the live prompt when it is already at the newest page.
 * The keyboard viewer is untouched and keeps its UP/DOWN/HOME/END/Q/ENTER
 * contract.
 */
static void pager_wheel(int32_t notches) {
    const uint32_t bottom = pager_bottom_top();
    int32_t step;
    uint32_t next;

    if (notches == 0) {
        return;
    }
    /* One report cannot move more than a page; a hostile/broken report is
     * bounded here instead of growing the work with its value. */
    if (notches > 8) {
        notches = 8;
    } else if (notches < -8) {
        notches = -8;
    }
    if (!pager_active) {
        if (notches < 0) {
            /* Already live at the newest line: wheel down has nothing to
             * scroll. */
            return;
        }
        pager_active = true;
        pager_top = bottom;
        pager_paint();
    }
    /* Positive wheel is away from the user, i.e. towards older lines. */
    step = -notches * (int32_t)CONSOLE_WHEEL_ROWS;
    if (step < 0) {
        const uint32_t up = (uint32_t)(-step);

        next = pager_top > up ? pager_top - up : 0U;
    } else {
        const uint32_t down = (uint32_t)step;

        if (pager_top >= bottom) {
            /* The newest page is already shown: the next notch returns to the
             * live prompt instead of pretending to scroll. */
            pager_leave();
            return;
        }
        next = pager_top + down;
        if (next > bottom) {
            next = bottom;
        }
    }
    if (next == pager_top) {
        return;
    }
    pager_top = next;
    pager_paint();
}

#ifdef HFOC_CONSOLE_HOST_TEST
uint32_t console_test_scrollback_line_count(void) {
    return scrollback_total_lines();
}

size_t console_test_scrollback_line_length(uint32_t offset) {
    const char *line = scrollback_line_at(offset);

    return line != NULL ? strlen(line) : 0U;
}

bool console_test_pager_active(void) {
    return pager_active;
}

uint32_t console_test_pager_top(void) {
    return pager_top;
}

uint32_t console_test_pager_bottom(void) {
    return pager_bottom_top();
}
#endif

static void scroll_command(void) {
    pager_active = true;
    pager_top = 0U;
    pager_paint();
}

static void console_emit(char value, bool to_serial) {
    /*
     * ITEM-483: output that is not the input echo - and not the deferred
     * echo's replay - is foreign to the line editor: it moves the cursor on
     * the screen and/or pushes the typed characters apart on the wire.  It
     * invalidates the one-cell fast path's assumption that the prompt and the
     * typed line still stand where they were left.  The counter is monotonic
     * and the fast path only compares it, so this needs no unwinding.
     */
    if (!console_replaying && !console_keyboard_echo_active) {
        ++console_output_generation;
    }
    /*
     * ITEM-460: the character the operator sees in the serial transcript is
     * also the character the durable transcript keeps, but only while a command
     * is running - the typed echo before ENTER is the secret being entered and
     * is not written down.
     */
    if (to_serial) {
        console_transcript_capture(value);
    }
    if (value == '\n' || value == '\r') {
        console_line_open = false;
    } else if (value != '\b') {
        console_line_open = true;
    }
    if (value == '\n') {
        const bool was_deferring = console_deferring;
        bool flushed = false;

        if (to_serial) {
            serial_putc('\n');
        }
        /*
         * ITEM-474: a newline that cannot be appended to a full buffer must
         * not be lost - the row it advances to is already counted.  Flush the
         * batch *before* the row is counted, so this newline becomes the first
         * overflow of the fresh batch and is represented by the pending count
         * instead of the buffer.
         */
        if (was_deferring && !console_replaying &&
            console_defer_length + 1U >= CONSOLE_DEFER_MAX) {
            console_flush_deferred();
            flushed = true;
        }
        /*
         * The cap flush inside new_line() also applied this newline's row, so
         * a newline it flushed must not be buffered a second time either.
         */
        if (new_line()) {
            flushed = true;
        }
        /*
         * The newline that opens a batch is accounted for by the pending row
         * count; every later one is buffered so the replay reaches the same
         * rows.  Replay itself never buffers.
         */
        if (was_deferring && !flushed && console_deferring &&
            !console_replaying) {
            (void)console_defer_push('\n');
        }
        return;
    }
    if (value == '\r') {
        if (to_serial) {
            serial_putc('\r');
        }
        if (console_deferring && !console_replaying &&
            console_defer_push('\r')) {
            return;
        }
        cursor_x = LEFT;
        return;
    }
    if (value == '\t') {
        for (unsigned int index = 0; index < 4U; ++index) {
            console_emit(' ', to_serial);
        }
        return;
    }
    if (value == '\b') {
        if (cursor_x > LEFT) {
            cursor_x -= console_cell_width;
            if (to_serial) {
                serial_write("\b \b");
            }
            /*
             * ITEM-474: the erase is deferred with the paint, but the echo is
             * serialized now so the transcript keeps every edit.
             */
            if (console_deferring && !console_replaying &&
                console_defer_push('\b')) {
                return;
            }
            console_note_echo_paint();
            erase_cell();
        }
        return;
    }

    if (cursor_x + console_cell_width > framebuffer_width()) {
        (void)new_line();
    }
    if (to_serial) {
        serial_putc(value);
    }
    /*
     * ITEM-474: once the batch is open the character is serialized now and
     * painted by the flush, so the turn's line loop copies text instead of
     * moving the 4K body once per line.  The logical column still advances -
     * the replay must see the same wraps, and every wrap is a row the one body
     * move has to cover - but nothing is painted until the flush.
     */
    if (console_deferring && !console_replaying) {
        if (console_defer_push(value)) {
            cursor_x += console_cell_width;
            return;
        }
    }
    /* ITEM-461: the character the paint counters see; the burst's own time is
       measured by console_write()/console_record_paint(). */
    ++console_paint_chars;
    console_note_echo_paint();
    erase_cell();
    framebuffer_char(cursor_x, cursor_y, value, TEXT, console_scale);
    cursor_x += console_cell_width;
}

/*
 * ITEM-509 increment 3: the console paints code points, not bytes.
 *
 * The output path decodes UTF-8 with the language catalog's own strict decoder
 * (`appliance_language_utf8_next()`): a valid sequence is one cell, and an
 * invalid or truncated one is one visible marker - never silence.  Wrapping and
 * the one-cell advance are counted per code point, so a Russian line breaks
 * where a Latin one of the same length would; the serial stream still carries
 * the original bytes, so every transcript, marker and proof that reads serial
 * sees exactly what it saw before.
 *
 * A code point the font does not carry is drawn as the framebuffer's marker and
 * published as one record naming it.  The record is the answer to "why is there
 * a box on the screen", and the coverage proof is what keeps it from firing in
 * normal operation.
 */
static size_t console_record_append_bytes(char *output, size_t capacity,
                                          size_t length, const char *text,
                                          size_t text_length) {
    size_t index;

    for (index = 0U; index < text_length && length + 1U < capacity; ++index) {
        output[length++] = text[index];
    }
    return length;
}

static size_t console_record_append_hex(char *output, size_t capacity,
                                        size_t length, uint32_t value,
                                        unsigned int digits) {
    static const char hex_digits[] = "0123456789ABCDEF";
    unsigned int position;

    for (position = 0U; position < digits; ++position) {
        const unsigned int shift = (digits - 1U - position) * 4U;

        if (length + 1U >= capacity) {
            break;
        }
        output[length++] = hex_digits[(value >> shift) & 0x0FU];
    }
    return length;
}

static size_t console_record_append_u64(char *output, size_t capacity,
                                        size_t length, uint64_t value) {
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U && length + 1U < capacity) {
        output[length++] = digits[--count];
    }
    return length;
}

/*
 * `invalid_byte` is the byte that broke a sequence, or -1 when the code point
 * itself simply has no glyph.  Both are one record each; a run of them carries
 * the running count, so a reader can tell one box from a screenful.
 */
static void console_publish_glyph_missing(uint32_t codepoint, int invalid_byte) {
    char record[CONSOLE_RECORD_LINE_MAX + 1U];
    char prefix[] = "[GLYPH-MISSING] codepoint=U+";
    size_t length = 0U;

    length = console_record_append_bytes(record, sizeof(record), length,
                                         prefix, sizeof(prefix) - 1U);
    length = console_record_append_hex(record, sizeof(record), length,
                                       codepoint, 4U);
    if (invalid_byte >= 0) {
        char broken[] = " byte=0x";
        length = console_record_append_bytes(record, sizeof(record), length,
                                             broken, sizeof(broken) - 1U);
        length = console_record_append_hex(record, sizeof(record), length,
                                           (uint32_t)invalid_byte, 2U);
    }
    {
        char counted[] = " count=";
        length = console_record_append_bytes(record, sizeof(record), length,
                                             counted, sizeof(counted) - 1U);
    }
    length = console_record_append_u64(record, sizeof(record), length,
                                       framebuffer_glyph_missing_count());
    console_journal_record(record, length);
}

/*
 * The printable half of console_emit(), by code point: the wrap, the serial
 * bytes, the deferred batch and the paint.  `console_emit()` above still owns
 * the control characters, which have no glyph by design.
 */
static void console_emit_codepoint(uint32_t codepoint, const char *bytes,
                                   size_t length, bool to_serial,
                                   int invalid_byte) {
    if (!console_replaying && !console_keyboard_echo_active) {
        ++console_output_generation;
    }
    if (cursor_x + console_cell_width > framebuffer_width()) {
        (void)new_line();
    }
    if (to_serial) {
        for (size_t index = 0U; index < length; ++index) {
            console_transcript_capture(bytes[index]);
            serial_putc(bytes[index]);
        }
    }
    if (console_deferring && !console_replaying) {
        if (console_defer_push_bytes(bytes, length)) {
            cursor_x += console_cell_width;
            return;
        }
    }
    {
        bool carried;

        ++console_paint_chars;
        console_note_echo_paint();
        erase_cell();
        carried = framebuffer_char_codepoint(cursor_x, cursor_y, codepoint,
                                             TEXT, console_scale);
        if (!carried) {
            /*
             * After the paint, so the record names the count the operator's
             * screen now shows.  A deferred batch publishes here too: the
             * replay is where its cell is actually drawn, and publishing at the
             * push would name a count the batch has not reached yet.
             */
            console_publish_glyph_missing(codepoint, invalid_byte);
        }
        cursor_x += console_cell_width;
    }
}

/*
 * A byte that cannot start or continue a sequence: one marker, one record.  The
 * replacement code point is what the marker stands for; the record keeps the
 * byte itself, because "which byte" is what a reader needs.
 */
static void console_emit_invalid_byte(unsigned char value) {
    char byte[1];

    byte[0] = (char)value;
    console_emit_codepoint(0xFFFDU, byte, 1U, true, (int)value);
}

void console_putc(char value) {
    if ((unsigned char)value >= 0x80U) {
        /*
         * A single high byte is a truncated sequence by definition: the console
         * never receives half of a code point from a string (console_write()
         * decodes whole strings) - only a caller that writes bytes one at a
         * time can produce one, and it is refused visibly.
         */
        console_emit_invalid_byte((unsigned char)value);
        return;
    }
    scrollback_capture_byte((unsigned char)value);
    console_emit(value, true);
    /*
     * ITEM-474: before the main loop opens the per-turn batch, a lone newline
     * applies its own overflow so the boot screen is never left behind a
     * pending move.  A printable fragment does not flush: a multi-write record
     * like the menu's action line must not have another record's geometry
     * published in the middle of it.  Inside console_write() the depth keeps
     * the flush to the burst's own end.
     */
    if (value == '\n' && !console_defer_batching &&
        console_write_depth == 0U && !console_flush_active) {
        console_flush_deferred();
    }
}

void console_write(const char *text) {
    uint64_t newlines = 0U;
    uint64_t started;
    uint64_t scroll_before = 0U;
    const char *cursor;

    /*
     * ITEM-461: one contiguous write that ends a line is a paint burst; the
     * whole call's time is charged to its lines.  A write with no newline (a
     * prompt, a fragment) is not a line and is not charged.  ITEM-463: the
     * scrolls this burst caused are subtracted, because they are charged to
     * `[CONSOLE] scroll` and a paint budget must bound the paint.
     */
    for (cursor = text; cursor != NULL && *cursor != '\0'; ++cursor) {
        if (*cursor == '\n') {
            ++newlines;
        }
        scrollback_capture_byte((unsigned char)*cursor);
    }
    if (newlines != 0U) {
        started = timer_cycles();
        scroll_before = console_scroll_cycles_total;
    } else {
        started = 0U;
    }
    /*
     * ITEM-448: the arrow is put away for the write and painted again after
     * it.  A software pointer saves the pixels it covers, so text written
     * underneath it would otherwise be restored away by the next hide - or
     * worse, the arrow would be saved as if it were screen content.
     */
    pointer_suspend();
    ++console_write_depth;
    /*
     * ITEM-509 increment 3: one write carries whole code points.
     *
     * The string is decoded here, not byte by byte in console_putc(), so a
     * Russian sequence is one cell: the decoder is the catalog's own strict
     * one, a valid sequence is painted as the code point it encodes, and a byte
     * that cannot start or continue one is refused visibly.  Every ASCII byte
     * takes the same path it always did.
     */
    {
        const size_t length = (size_t)(cursor - text);
        size_t offset = 0U;

        while (offset < length) {
            const unsigned char byte = (unsigned char)text[offset];
            uint32_t codepoint = 0U;
            size_t next = offset;

            if (byte < 0x80U) {
                console_putc((char)byte);
                ++offset;
                continue;
            }
            if (appliance_language_utf8_next(text, length, &next,
                                             &codepoint)) {
                console_emit_codepoint(codepoint, text + offset,
                                       next - offset, true, -1);
                offset = next;
                continue;
            }
            console_emit_invalid_byte(byte);
            ++offset;
        }
    }
    --console_write_depth;
    pointer_resume();
    /*
     * ITEM-464: the framebuffer is write-combining, so the stores this write
     * queued must reach the device before the duration is read; otherwise the
     * measured time is the queueing, not the frame the operator waits for.
     */
    console_sync();
    if (newlines != 0U) {
        const uint64_t spent =
            timer_cycles() - started - (console_scroll_cycles_total - scroll_before);

        console_paint_record_lines(newlines, spent);
    }
    /*
     * ITEM-474: outside the main loop's per-turn batch, a burst that ends a
     * line applies its own scroll now, so a boot message or a host proof sees
     * the screen it wrote.  Inside the batch the move waits for the turn's
     * flush.  A fragment with no newline is not a burst and does not flush -
     * that is what keeps a record assembled from several writes whole.
     */
    if (newlines != 0U && !console_defer_batching && !console_flush_active) {
        console_flush_deferred();
    }
}

static void write_u64(uint64_t value) {
    char buffer[21];
    size_t length = 0;
    if (value == 0U) {
        console_putc('0');
        return;
    }
    while (value != 0U) {
        buffer[length++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (length != 0U) {
        console_putc(buffer[--length]);
    }
}

static void prompt(void) {
    /*
     * ITEM-474: the command's output ends with a newline, so a batch it left
     * open is applied here, before the prompt is appended to the last output
     * line.  This keeps the prompt on its own line and the `[CONSOLE] scroll`
     * record the flush may publish on a line of its own - the serial shape
     * ITEM-457/463 pinned.
     */
    console_flush_deferred();
    console_management_flush();
    console_write("hfoc> ");
}

static void redraw_input_line(void) {
    /*
     * ITEM-461: the whole redraw is one serial sequence.  The typed line is the
     * delivery witness the console proofs use (ITEM-427), and a long line split
     * by an IRQ trace line is unreadable as one echo: the 2026-09-20 load proof
     * lost the third of three 80-character lines exactly that way.  The write
     * is bounded by INPUT_MAX, and the same class of fix ITEM-440 made for the
     * menu's own lines.
     */
    const uint64_t flags = serial_sequence_begin();
    /*
     * MOUSE-DIAG (owner task 2026-09-27): the keyboard contract asked how often
     * a keystroke costs a whole line instead of one cell, how many characters
     * that repaints and how long it takes - three numbers the run's telemetry
     * did not carry.  They are counted here, around the redraw only, and change
     * nothing about what it draws.
     */
    const uint64_t started = timer_cycles();

    console_write("\r");
    prompt();
    for (size_t index = 0U; index < input_length; ++index) {
        console_putc(input_line[index]);
    }
    if (previous_rendered_length > input_length) {
        console_putc(' ');
        for (size_t index = 0U;
             index < previous_rendered_length - input_length; ++index) {
            console_putc('\b');
        }
    }
    for (size_t index = input_length; index > input_cursor; --index) {
        console_putc('\b');
    }
    previous_rendered_length = input_length;
    /* ITEM-483: the line is whole on both surfaces again, so the fast path may
       paint one cell for as long as nobody else moves the screen. */
    console_input_line_generation = console_output_generation;
    serial_sequence_end(flags);
    {
        const uint64_t micros = timer_cycles_to_us(timer_cycles() - started);

        ++console_kbd_echo_redraw_count;
        console_kbd_echo_redraw_chars += input_length;
        console_kbd_echo_redraw_us_total += micros;
        if (micros > console_kbd_echo_redraw_us_max) {
            console_kbd_echo_redraw_us_max = micros;
        }
    }
}

static void clear_body(void) {
    /* ITEM-482: a body repaint covers whatever the pointer had saved, so the
       pointer is hidden first and redrawn afterwards - the same rule the
       scroll follows. */
    pointer_suspend();
    framebuffer_rect(0, BODY_TOP, framebuffer_width(), framebuffer_height() - BODY_TOP, BACKGROUND);
    cursor_x = LEFT;
    cursor_y = BODY_TOP + 8U;
    pointer_resume();
    /* ITEM-483: the body is gone and the cursor was reset, so the screen is no
       longer the one the fast path left behind. */
    ++console_output_generation;
}

/* Read-only page-table walk for VMM PTE diagnostics.
   Returns PTE value, or 0 if walk encounters unexpected structure.
   Does NOT write page tables, MSR, or APIC registers. */
static uint64_t vmm_read_pte_diagnostic(uint64_t phys_addr, uint64_t hhdm) {
    uint64_t cr3_phys, *pml4, pml4e, *pdpt, pdpte, *pd, pde, *pt, pte;
    uint64_t vaddr = phys_addr + hhdm;
    uint64_t idx;

    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_phys));
    cr3_phys &= 0x000FFFFFFFFFF000ULL;
    pml4 = (uint64_t *)(cr3_phys + hhdm);

    idx = (vaddr >> 39) & 0x1FF;
    pml4e = pml4[idx];
    if (!(pml4e & 1)) return 0;

    pdpt = (uint64_t *)((pml4e & 0x000FFFFFFFFFF000ULL) + hhdm);
    idx = (vaddr >> 30) & 0x1FF;
    pdpte = pdpt[idx];
    if (!(pdpte & 1)) return 0;
    if (pdpte & (1ULL << 7)) return 0;  /* 1 GiB large page */

    pd = (uint64_t *)((pdpte & 0x000FFFFFFFFFF000ULL) + hhdm);
    idx = (vaddr >> 21) & 0x1FF;
    pde = pd[idx];
    if (!(pde & 1)) return 0;
    if (pde & (1ULL << 7)) return 0;  /* 2 MiB large page */

    pt = (uint64_t *)((pde & 0x000FFFFFFFFFF000ULL) + hhdm);
    idx = (vaddr >> 12) & 0x1FF;
    pte = pt[idx];
    if (!(pte & 1)) return 0;

    return pte;
}

/*
 * ITEM-506: the one account on the local console.
 *
 * The conversation is a pure state machine (`appliance_admin_credentials_prompt`)
 * and this is its only console side: it owns the hidden line, renders each
 * question, and hands the finished answers to the bound enrollment owner.  The
 * owner is the only thing that verifies, provisions, authenticates or resets,
 * so the console holds no policy and no secret beyond the line being typed -
 * which is zeroed after every step, never echoed, never pushed to the history
 * and never written to the transcript.
 *
 * The glue is compiled into the host console proof as well as the kernel (the
 * enrollment owner itself is production-only), so the hidden input, the
 * command vocabulary and the refusals are provable without QEMU.
 */
static const struct console_admin_credentials_binding
    *admin_credentials_binding;
static const struct console_entropy_login_binding *entropy_login_binding;
static struct appliance_admin_credentials_prompt admin_credentials_prompt;
static bool entropy_login_auto;
/* ITEM-585: the hidden operator-entropy line.  It shares the line buffer with
   the account conversation but never the command history or the transcript. */
/*
 * ITEM-465/ITEM-506: the armed half of the reset gesture, now under the one
 * `credentials` command.  It is set by `CREDENTIALS RESET` and consumed by the
 * confirmation; any other submitted line clears it, so the confirmation must
 * immediately follow the request.
 */
static bool credentials_reset_pending;

static void console_admin_credentials_zero_line(void);

void console_bind_admin_credentials(
    const struct console_admin_credentials_binding *binding) {
    admin_credentials_binding = binding;
    if (binding == NULL) {
        admin_credentials_active = false;
        appliance_admin_credentials_prompt_reset(&admin_credentials_prompt);
        /*
         * A binding that goes away must not leave a half-typed password in the
         * line buffer for the ordinary editor to redraw and submit.
         */
        console_admin_credentials_zero_line();
    }
}

void console_bind_entropy_login(
    const struct console_entropy_login_binding *binding) {
    entropy_login_binding = binding;
    if (binding == NULL) {
        entropy_login_auto = false;
    }
}

bool console_admin_credentials_input_active(void) {
    return admin_credentials_active;
}

/*
 * ITEM-509 increment 4: the console's language, its setting and its record.
 *
 * Increment 3 made the console able to draw Russian; this is the command that
 * chooses it.  The console owns the vocabulary and the answers; the system
 * record belongs to the owner behind `console_language_binding`.  Three rules
 * the item asks for are visible here:
 *
 *  * the answer is in the language that is active *after* the command, so a
 *    successful switch confirms itself in the new language;
 *  * a value the catalog does not name is refused with the usage line - never
 *    treated as English, never silently ignored;
 *  * a change is one durable write plus one journal record, and a machine whose
 *    record cannot be written says so instead of reporting a change that did
 *    not happen.
 *
 * The console keeps the published language and updates it only after the
 * owner's write says yes; it never reads the record itself, so there is one
 * source of truth and no second copy to drift.
 */
static const struct console_language_binding *language_binding;
static enum appliance_language console_language = APPLIANCE_LANGUAGE_DEFAULT;
static bool console_language_durable;

void console_bind_language(const struct console_language_binding *binding) {
    language_binding = binding;
}

void console_publish_language(enum appliance_language language, bool durable) {
    if (language != APPLIANCE_LANGUAGE_EN && language != APPLIANCE_LANGUAGE_RU) {
        return;
    }
    console_language = language;
    console_language_durable = durable;
}

enum appliance_language console_active_language(void) {
    return console_language;
}

bool console_language_is_durable(void) {
    return console_language_durable;
}

/*
 * The answers are printed one whole line at a time (`console_write_atomic()`):
 * a serial capture that interleaves an IRQ trace record between a catalog word
 * and the language code would show `LANGUAGE` and `: EN` as two unrelated
 * lines, and the guest proof - and the operator reading a captured transcript -
 * needs the answer as the one line the screen shows.
 */
#define CONSOLE_LANGUAGE_LINE_MAX 96U

static void console_language_write_line(const char *text) {
    char line[CONSOLE_LANGUAGE_LINE_MAX];
    size_t length = 0U;

    if (text == NULL) {
        return;
    }
    while (text[length] != '\0' && length + 1U < sizeof(line)) {
        line[length] = text[length];
        ++length;
    }
    line[length++] = '\n';
    line[length] = '\0';
    console_write_atomic(line);
}

/* One catalog line in the active language, or nothing when the key is unknown
   (the coverage proof is what keeps that from happening). */
static void console_language_write_key(const char *key) {
    console_language_write_line(
        appliance_language_string(console_language, key));
}

/* `LANGUAGE: EN` / `ЯЗЫК: RU` - the catalog's word, the code in capitals. */
static void console_language_write_show(void) {
    char line[CONSOLE_LANGUAGE_LINE_MAX];
    const char *text = appliance_language_string(
        console_language, "console.language.current");
    const char *code = appliance_language_code(console_language);
    size_t length = 0U;

    if (text == NULL || code == NULL) {
        return;
    }
    while (*text != '\0' && length + 1U < sizeof(line)) {
        line[length++] = *text++;
    }
    if (length + 3U < sizeof(line)) {
        line[length++] = ':';
        line[length++] = ' ';
    }
    while (*code != '\0' && length + 1U < sizeof(line)) {
        char value = *code++;

        if (value >= 'a' && value <= 'z') {
            value = (char)(value - 'a' + 'A');
        }
        line[length++] = value;
    }
    line[length++] = '\n';
    line[length] = '\0';
    console_write_atomic(line);
}

/*
 * The machine record of one language command.  English, like every other
 * journal record: the value that was asked for, and - when nothing was written
 * - the reason, so a reader can tell a change from a refusal without the
 * screen.
 */
static void console_language_record(const char *value, size_t value_length,
                                    const char *reason) {
    char record[CONSOLE_RECORD_LINE_MAX + 1U];
    size_t length = 0U;
    static const char prefix[] = "[CONSOLE] language=";
    static const char refused[] = " refused reason=";

    length = console_record_append_bytes(record, sizeof(record), length,
                                         prefix, sizeof(prefix) - 1U);
    if (value != NULL) {
        length = console_record_append_bytes(record, sizeof(record), length,
                                             value, value_length);
    }
    if (reason != NULL) {
        length = console_record_append_bytes(record, sizeof(record), length,
                                             refused, sizeof(refused) - 1U);
        length = console_record_append_bytes(record, sizeof(record), length,
                                             reason, strlen(reason));
    }
    console_journal_record(record, length);
}

/* The record's value is the language's own code, or nothing at all. */
static void console_language_record_code(enum appliance_language language,
                                         const char *reason) {
    const char *code = appliance_language_code(language);

    console_language_record(code, code != NULL ? strlen(code) : 0U, reason);
}

static void console_language_command(enum console_language_command_kind kind,
                                     enum appliance_language requested,
                                     const char *value,
                                     size_t value_length) {
    if (kind == CONSOLE_LANGUAGE_COMMAND_SHOW) {
        console_language_write_show();
        return;
    }
    if (kind == CONSOLE_LANGUAGE_COMMAND_REFUSE) {
        console_language_write_key("console.language.refused");
        console_language_write_key("console.language.usage");
        console_language_record(value, value_length, "UNKNOWN_LANGUAGE");
        return;
    }
    if (language_binding == NULL || language_binding->write == NULL ||
        !console_language_durable) {
        console_language_write_key("console.language.unavailable");
        console_language_record_code(requested, "UNAVAILABLE");
        return;
    }
    if (!language_binding->write(language_binding->context, requested)) {
        console_language_write_key("console.language.unavailable");
        console_language_record_code(requested, "WRITE_FAILED");
        return;
    }
    console_language = requested;
    console_language_record_code(requested, NULL);
    console_language_write_key("console.language.saved");
}

/*
 * ITEM-516: the local half of the management gate.
 *
 * `gate` shows the setting, `gate close` writes the safe default, and
 * `gate open` prints the full warning and arms a confirmation.  Enabling is
 * local-only by construction: the command is only here, and the confirmation
 * additionally requires that this console session proved the administrator
 * secret (`login`, or the current-password step of `credentials`).  Nothing
 * is written by the request; the confirmation writes, and it must come on the
 * very next line, exactly like the ITEM-465/506 reset gesture.
 *
 * Every answer goes through `console_write_atomic()` for the same reason the
 * language answers do: a record that interleaves with an IRQ trace would make
 * the screen and the machine log disagree.
 */
static const struct console_gate_binding *gate_binding;
static bool console_gate_open_setting;
static bool console_gate_durable;
static bool console_gate_open_armed;
static bool console_gate_proven;

void console_bind_gate(const struct console_gate_binding *binding) {
    gate_binding = binding;
    if (binding == NULL) {
        console_gate_open_armed = false;
    }
}

void console_publish_gate(bool open, bool durable) {
    console_gate_open_setting = open;
    console_gate_durable = durable;
}

bool console_gate_is_open(void) {
    return console_gate_open_setting;
}

bool console_gate_is_durable(void) {
    return console_gate_durable;
}

bool console_gate_local_secret_proven(void) {
    return console_gate_proven;
}

/*
 * The local proof is set by the two conversations that verify the account's
 * secret on this machine and nowhere else: a successful `login`, and the
 * current-password step of `credentials`.  A remote panel sign-in publishes
 * the verifier but never touches this flag, so `gate open` stays local-only.
 */
static void console_gate_record_local_proof(void) {
    console_gate_proven = true;
}

static void console_gate_write_line(const char *text) {
    /*
     * The gate's answers are whole operator sentences - the warning is the
     * item's contract text - so they are bounded by the console's own record
     * bound rather than by the short language answers' 96 bytes.  A Russian
     * warning is about 230 bytes, and truncating it would hide exactly the
     * sentence the operator must read.
     */
    char line[CONSOLE_RECORD_MAX + 1U];

    if (text == NULL) {
        return;
    }
    {
        size_t length = 0U;

        while (text[length] != '\0' && length + 1U < sizeof(line)) {
            line[length] = text[length];
            ++length;
        }
        line[length++] = '\n';
        line[length] = '\0';
    }
    console_write_atomic(line);
}

static void console_gate_write_key(const char *key) {
    console_gate_write_line(
        appliance_language_string(console_active_language(), key));
}

/*
 * ITEM-522: the gate's machine records are one IRQ-safe write, like the menu
 * lines (ITEM-440/443) and the network answers (ITEM-496).  They used the
 * plain writer, and the 2026-10-12 acceptance run showed what that costs: an
 * `[IOAPIC] irq1` trace landed inside `[PRODUCTION-SECURITY] gate=ENABLED
 * source=LOCAL_SECRET`, so the record read back from the serial as
 * `PRODUCTION-SECURITY] ...` and the proof's exact grep could not find it -
 * the write the operator saw on the screen was unreadable in the log, which is
 * the one place a hardware run can be read at all.
 */
static void console_gate_record(const char *record) {
    char line[CONSOLE_RECORD_MAX + 2U];
    size_t length = 0U;

    if (record == NULL) {
        return;
    }
    while (record[length] != '\0' && length + 2U < sizeof(line)) {
        line[length] = record[length];
        ++length;
    }
    line[length++] = '\n';
    line[length] = '\0';
    console_write_atomic(line);
    console_journal_record(record, strlen(record));
}

/* `GATE: OPEN` / `ВОРОТА: ОТКРЫТЫ` - the catalog's word, the machine value. */
static void console_gate_show(void) {
    char line[CONSOLE_LANGUAGE_LINE_MAX];
    const char *text =
        appliance_language_string(console_active_language(),
                                  "console.gate.current");
    const char *value =
        console_gate_open_setting
            ? appliance_language_string(console_active_language(),
                                        "console.gate.value_open")
            : appliance_language_string(console_active_language(),
                                        "console.gate.value_closed");
    size_t length = 0U;

    if (text == NULL || value == NULL) {
        return;
    }
    while (*text != '\0' && length + 1U < sizeof(line)) {
        line[length++] = *text++;
    }
    if (length + 3U < sizeof(line)) {
        line[length++] = ':';
        line[length++] = ' ';
    }
    while (*value != '\0' && length + 1U < sizeof(line)) {
        line[length++] = *value++;
    }
    line[length] = '\0';
    console_gate_write_line(line);
    /*
     * The setting's meaning is the warning itself, so a reader who only asks
     * for the state still learns what an open gate means.
     */
    console_gate_write_key(console_gate_open_setting
                               ? "console.gate.warning_short_open"
                               : "console.gate.warning_short_closed");
}

/*
 * The full warning the operator must read before enabling.  It names the
 * physical-presence consequence, the two things left protecting the panel
 * (the password and the failure lockout) and audit finding R01: the
 * management service listens on a wildcard address, so an appliance exposed
 * to the Internet exposes the panel to the whole network.
 */
static void console_gate_warning(void) {
    console_gate_write_key("console.gate.warning_title");
    console_gate_write_key("console.gate.warning_no_presence");
    console_gate_write_key("console.gate.warning_password_only");
    console_gate_write_key("console.gate.warning_lockout");
    console_gate_write_key("console.gate.warning_wildcard");
    console_gate_write_key("console.gate.warning_internet");
    console_gate_write_key("console.gate.warning_confirm");
}

static void console_gate_command(enum console_gate_command_kind kind) {
    if (kind == CONSOLE_GATE_COMMAND_SHOW) {
        console_gate_show();
        return;
    }
    if (kind == CONSOLE_GATE_COMMAND_REFUSE) {
        console_gate_write_key("console.gate.refused");
        console_gate_write_key("console.gate.usage");
        console_gate_record(appliance_panel_gate_record(
            APPLIANCE_PANEL_GATE_EVENT_REFUSED));
        return;
    }
    if (kind == CONSOLE_GATE_COMMAND_OPEN_REQUEST) {
        if (!console_gate_proven) {
            console_gate_write_key("console.gate.local_secret_required");
            console_gate_write_key("console.gate.usage");
            console_gate_record(appliance_panel_gate_record(
                APPLIANCE_PANEL_GATE_EVENT_REFUSED));
            return;
        }
        console_gate_warning();
        console_gate_record(appliance_panel_gate_record(
            APPLIANCE_PANEL_GATE_EVENT_PENDING));
        return;
    }
    if (kind == CONSOLE_GATE_COMMAND_OPEN_UNCONFIRMED) {
        console_gate_write_key("console.gate.unconfirmed");
        console_gate_record(appliance_panel_gate_record(
            APPLIANCE_PANEL_GATE_EVENT_REFUSED));
        return;
    }
    if (kind == CONSOLE_GATE_COMMAND_OPEN_CONFIRM) {
        if (!console_gate_proven) {
            console_gate_write_key("console.gate.local_secret_required");
            console_gate_record(appliance_panel_gate_record(
                APPLIANCE_PANEL_GATE_EVENT_REFUSED));
            return;
        }
        if (gate_binding == NULL || gate_binding->write == NULL ||
            !console_gate_durable) {
            console_gate_write_key("console.gate.unavailable");
            console_gate_record(appliance_panel_gate_record(
                APPLIANCE_PANEL_GATE_EVENT_REFUSED));
            return;
        }
        if (!gate_binding->write(gate_binding->context, true)) {
            console_gate_write_key("console.gate.write_failed");
            console_gate_record(appliance_panel_gate_record(
                APPLIANCE_PANEL_GATE_EVENT_REFUSED));
            return;
        }
        console_gate_open_setting = true;
        console_gate_record(appliance_panel_gate_setting_record(true));
        console_gate_write_key("console.gate.saved_open");
        return;
    }
    if (kind == CONSOLE_GATE_COMMAND_CLOSE) {
        if (gate_binding == NULL || gate_binding->write == NULL ||
            !console_gate_durable) {
            console_gate_write_key("console.gate.unavailable");
            console_gate_record(appliance_panel_gate_record(
                APPLIANCE_PANEL_GATE_EVENT_REFUSED));
            return;
        }
        if (!gate_binding->write(gate_binding->context, false)) {
            console_gate_write_key("console.gate.write_failed");
            console_gate_record(appliance_panel_gate_record(
                APPLIANCE_PANEL_GATE_EVENT_REFUSED));
            return;
        }
        console_gate_open_setting = false;
        console_gate_record(appliance_panel_gate_setting_record(false));
        console_gate_write_key("console.gate.saved_closed");
        return;
    }
}

static const char *console_admin_credentials_status_text(
    enum appliance_production_admin_enrollment_status status) {
    if (admin_credentials_binding != NULL &&
        admin_credentials_binding->status_text != NULL) {
        return admin_credentials_binding->status_text(status);
    }
    return "UNKNOWN";
}

static void console_admin_credentials_zero_line(void) {
    for (size_t index = 0U; index <= INPUT_MAX; ++index) {
        input_line[index] = '\0';
    }
    input_length = 0U;
    input_cursor = 0U;
    input_overflow = false;
}

static void console_admin_credentials_end(void) {
    appliance_admin_credentials_prompt_reset(&admin_credentials_prompt);
    admin_credentials_active = false;
    entropy_login_auto = false;
}

/*
 * ITEM-585: the local operator is the explicit entropy source on a CPU with
 * neither RDRAND nor RDSEED.  The seed is typed on a hidden line, accepted only
 * when it is 64 hex characters and 32 diverse bytes, mixed through the same
 * chain as a hardware block, and then zeroed.  The prompt never prints the
 * material and the command name alone reaches the history/journal.
 */
static void console_entropy_seed_begin(void) {
    if (entropy_seed_active) {
        console_write("[RNG] operator seed conversation already open\n");
        return;
    }
#ifdef HFOC_PRODUCTION_TLS
    if (appliance_production_entropy_ready()) {
        console_write("[RNG] operator seed refused: hardware entropy is READY\n");
        return;
    }
    if (appliance_production_entropy_state() !=
            APPLIANCE_PRODUCTION_ENTROPY_STATE_UNAVAILABLE &&
        appliance_production_entropy_state() !=
            APPLIANCE_PRODUCTION_ENTROPY_STATE_FAILED) {
        console_write("[RNG] operator seed refused: entropy owner is neither "
                      "AWAITING_ENTROPY nor FAILED\n");
        return;
    }
    entropy_seed_active = true;
    input_length = 0U;
    input_cursor = 0U;
    input_line[0] = '\0';
    input_overflow = false;
    console_write("[RNG] operator seed: type 64 hex characters, then ENTER\n");
#else
    console_write("[RNG] operator seed unavailable in this profile\n");
#endif
}

static void console_entropy_seed_zero_line(void) {
    size_t index;

    for (index = 0U; index <= INPUT_MAX; ++index) {
        input_line[index] = '\0';
    }
    input_length = 0U;
    input_cursor = 0U;
    input_overflow = false;
}

static void console_entropy_seed_enter(void) {
#ifdef HFOC_PRODUCTION_TLS
    uint8_t seed[APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES];
    enum appliance_production_entropy_seed_status status;
    const size_t typed_length = input_length;

    memset(seed, 0, sizeof(seed));
    if (input_overflow) {
        status = APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH;
    } else {
        status = appliance_production_entropy_operator_seed_status_from_hex(
            input_line, typed_length, seed);
    }
    if (status == APPLIANCE_PRODUCTION_ENTROPY_SEED_OK) {
        status = appliance_production_entropy_seed_operator(
            seed, sizeof(seed), hfoc_lwip_time_now());
    }
    memset(seed, 0, sizeof(seed));
    console_entropy_seed_zero_line();
    entropy_seed_active = false;
    console_write("[RNG] operator seed status=");
    console_write(appliance_production_entropy_seed_status_string(status));
    if (status == APPLIANCE_PRODUCTION_ENTROPY_SEED_OK) {
        console_write(" source=");
        console_write(appliance_production_entropy_source_string());
        console_write(" state=");
        console_write(appliance_production_entropy_state_string(
            appliance_production_entropy_state()));
        console_write(" credited=");
        write_u64(appliance_production_entropy_credited_bytes());
    }
    console_putc('\n');
#else
    (void)console_entropy_seed_zero_line;
#endif
    prompt();
}

static void console_admin_credentials_state(void) {
    if (admin_credentials_binding != NULL &&
        admin_credentials_binding->status != NULL) {
        admin_credentials_binding->status(admin_credentials_binding->context);
    }
}

static struct console_entropy_login_result console_entropy_login_result(
    enum console_entropy_login_stage stage) {
    struct console_entropy_login_result result;

    memset(&result, 0, sizeof(result));
    result.stage = stage;
    result.seed_status = APPLIANCE_HOST_SEED_UNSUPPORTED;
    return result;
}

static const char *console_entropy_login_stage_string(
    enum console_entropy_login_stage stage) {
    switch (stage) {
    case CONSOLE_ENTROPY_LOGIN_STAGE_CONFIGURATION:
        return "CONFIGURATION";
    case CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT:
        return "ACCOUNT";
    case CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT_PUBLISH:
        return "ACCOUNT_PUBLISH";
    case CONSOLE_ENTROPY_LOGIN_STAGE_SEED:
        return "SEED";
    case CONSOLE_ENTROPY_LOGIN_STAGE_ENTROPY:
        return "ENTROPY";
    case CONSOLE_ENTROPY_LOGIN_STAGE_COMPLETE:
        return "COMPLETE";
    default:
        return "UNKNOWN";
    }
}

static const char *console_admin_enrollment_short_status(
    enum appliance_production_admin_enrollment_status status) {
    static const char prefix[] =
        "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_";
    const char *name =
        appliance_production_admin_enrollment_status_string(status);
    size_t index;

    if (name == NULL) {
        return "UNKNOWN";
    }
    for (index = 0U; index + 1U < sizeof(prefix); ++index) {
        if (name[index] != prefix[index]) {
            return name;
        }
    }
    return name + sizeof(prefix) - 1U;
}

static const char *console_entropy_login_status(
    const struct console_entropy_login_result *result) {
    if (result == NULL) {
        return "UNKNOWN";
    }
    switch (result->stage) {
    case CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT:
    case CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT_PUBLISH:
        return console_admin_enrollment_short_status(result->account_status);
    case CONSOLE_ENTROPY_LOGIN_STAGE_SEED:
    case CONSOLE_ENTROPY_LOGIN_STAGE_COMPLETE:
        return appliance_host_seed_status_string(result->seed_status);
    case CONSOLE_ENTROPY_LOGIN_STAGE_ENTROPY:
#ifdef HFOC_PRODUCTION_TLS
        return appliance_production_entropy_seed_status_string(
            result->entropy_status);
#else
        return "UNSUPPORTED";
#endif
    case CONSOLE_ENTROPY_LOGIN_STAGE_CONFIGURATION:
    default:
        return appliance_host_seed_status_string(result->seed_status);
    }
}

static void console_admin_credentials_begin(
    enum appliance_admin_credentials_prompt_mode mode) {
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;

    if (admin_credentials_binding == NULL) {
        console_write("[ADMIN-ENROLLMENT] UNAVAILABLE\n");
        return;
    }
    if (admin_credentials_active) {
        /*
         * ITEM-509 increment 24: the `[ADMIN-ENROLLMENT]` marker is a record
         * and stays English; the sentence after it is operator prose and
         * follows the chosen language.  Marker and sentence are one record
         * line, so they are written as one IRQ-safe sequence.
         */
        const uint64_t flags = serial_sequence_begin();

        console_write("[ADMIN-ENROLLMENT] ");
        console_write(
            console_local_menu_text("console.prose.credentials_busy"));
        serial_sequence_end(flags);
        return;
    }
    {
        const enum appliance_admin_credentials_prompt_status status =
            appliance_admin_credentials_prompt_start(
                &admin_credentials_prompt, mode, text, sizeof(text),
                &text_length, console_active_language());

        if (status != APPLIANCE_ADMIN_CREDENTIALS_PROMPT_OK) {
            /*
             * ITEM-509 increment 20: a catalog that cannot answer is the named
             * refusal the module wrote into `text`, not a silent English
             * question.
             */
            if (status ==
                    APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION &&
                text_length != 0U) {
                console_write_atomic(text);
                console_putc('\n');
            } else {
                console_write("[ADMIN-ENROLLMENT] conversation refused\n");
            }
            return;
        }
    }
    admin_credentials_active = true;
    /*
     * The state record goes first, exactly as the old `ADMIN` command answered
     * it: the operator about to change the account reads what the appliance
     * currently holds, and the acceptance runbook reads the same line.
     */
    console_admin_credentials_state();
    console_write_atomic(text);
}

static void console_entropy_unlock_commit(void) {
    size_t login_length = 0U;
    size_t password_length = 0U;
    const char *login = appliance_admin_credentials_prompt_login(
        &admin_credentials_prompt, &login_length);
    const char *password = appliance_admin_credentials_prompt_password(
        &admin_credentials_prompt, &password_length);
    struct console_entropy_login_result result =
        console_entropy_login_result(
            CONSOLE_ENTROPY_LOGIN_STAGE_CONFIGURATION);
    bool success;

    if (entropy_login_binding != NULL &&
        entropy_login_binding->unlock != NULL &&
        login != NULL && password != NULL) {
        result = entropy_login_binding->unlock(
            entropy_login_binding->context, login, login_length,
            password, password_length, (uint64_t)hfoc_lwip_time_now());
    }
    console_admin_credentials_end();
    success = result.stage == CONSOLE_ENTROPY_LOGIN_STAGE_COMPLETE &&
              result.seed_status == APPLIANCE_HOST_SEED_OK;
    if (success) {
        console_write("[RNG] host seed status=OK stage=COMPLETE");
    } else {
        console_write("[RNG] host unlock stage=");
        console_write(console_entropy_login_stage_string(result.stage));
        console_write(" status=");
        console_write(console_entropy_login_status(&result));
    }
#ifdef HFOC_PRODUCTION_TLS
    if (success) {
        console_write(" source=");
        console_write(appliance_production_entropy_source_string());
        console_write(" state=");
        console_write(appliance_production_entropy_state_string(
            appliance_production_entropy_state()));
        console_write(" credited=");
        write_u64(appliance_production_entropy_credited_bytes());
    }
#endif
    console_putc('\n');
    if (!success) {
        console_write(
            "[RNG] type `entropy unlock` to retry the local login\n");
    }
    prompt();
}

void console_start_entropy_unlock(void) {
    if (entropy_login_binding == NULL || admin_credentials_binding == NULL ||
        admin_credentials_active) {
        return;
    }
#ifdef HFOC_PRODUCTION_TLS
    if (appliance_production_entropy_ready()) {
        return;
    }
#endif
    entropy_login_auto = true;
    console_admin_credentials_begin(
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN);
    if (!admin_credentials_active) {
        entropy_login_auto = false;
    }
}

static void console_entropy_unlock_begin(void) {
    if (entropy_login_binding == NULL) {
        console_write("[RNG] host seed login is unavailable in this profile\n");
        return;
    }
#ifdef HFOC_PRODUCTION_TLS
    if (appliance_production_entropy_ready()) {
        console_write("[RNG] host seed login is not needed: entropy is READY\n");
        return;
    }
#endif
    if (admin_credentials_active) {
        console_write("[RNG] credential conversation already open\n");
        return;
    }
    console_start_entropy_unlock();
}

static void console_admin_credentials_commit(void) {
    const char *current;
    const char *login;
    const char *password;
    char changed_login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
    size_t current_length = 0U;
    size_t login_length = 0U;
    size_t password_length = 0U;
    const uint64_t now_tick = (uint64_t)hfoc_lwip_time_now();
    enum appliance_production_admin_enrollment_status status;
    bool existing_account = false;

    current = appliance_admin_credentials_prompt_current(
        &admin_credentials_prompt, &current_length);
    login = appliance_admin_credentials_prompt_login(
        &admin_credentials_prompt, &login_length);
    password = appliance_admin_credentials_prompt_password(
        &admin_credentials_prompt, &password_length);
    if (current == NULL || login == NULL || password == NULL) {
        console_write("[ADMIN-ENROLLMENT] conversation lost\n");
        console_admin_credentials_end();
        prompt();
        return;
    }
    /*
     * The ended conversation zeroes the buffer the login points into, so the
     * name the record is going to publish is copied out first; the password is
     * never copied and never printed.
     */
    for (size_t index = 0U; index < login_length; ++index) {
        changed_login[index] = login[index];
    }
    changed_login[login_length] = '\0';
    /*
     * The current password is proven before anything is written; a caller that
     * skipped this step would turn `credentials` into an unauthenticated
     * account takeover, so the check is here and not in the owner.
     *
     * One state has no password to prove: a fresh appliance whose account was
     * never set (`UNPROVISIONED`, the factory placeholder).  That is the
     * explicit first-set step, and it bypasses nothing - there was no usable
     * credential to protect.
     *
     * A legacy record is not that state: an operator set that secret once, so
     * it must not be overwritten without proof.  The change is refused here and
     * the operator takes the documented break-glass gesture (`credentials
     * reset`, then `credentials reset confirm`) before setting the new account.
     * Treating it as first-set would let anyone at the console overwrite a
     * credential they never proved.
     */
    status = admin_credentials_binding->verify(
        admin_credentials_binding->context, current, current_length, now_tick);
    if (status == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED) {
        static const char first_set_record[] =
            "[ADMIN-ENROLLMENT] no verifiable account; this sets the first "
            "one";

        console_write(first_set_record);
        console_putc('\n');
        console_journal_record(first_set_record, strlen(first_set_record));
    } else if (status == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD) {
        static const char legacy_record[] =
            "[ADMIN-ENROLLMENT] the old secret record is not migrated by a "
            "change; run `credentials reset`, then `credentials reset "
            "confirm`, and set the account again";

        console_write(legacy_record);
        console_putc('\n');
        console_journal_record(legacy_record, strlen(legacy_record));
        console_admin_credentials_end();
        console_admin_credentials_state();
        prompt();
        return;
    } else if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        console_write("[ADMIN-ENROLLMENT] current password refused: ");
        console_write(console_admin_credentials_status_text(status));
        console_putc('\n');
        console_admin_credentials_end();
        console_admin_credentials_state();
        prompt();
        return;
    } else {
        /*
         * ITEM-516: the current password was just proven on this machine, so
         * the gate's dangerous direction (`gate open confirm`) is unlocked for
         * this console session.
         */
        console_gate_record_local_proof();
        existing_account = true;
    }
    if (existing_account && admin_credentials_binding->rekey_credential !=
                                NULL &&
        !admin_credentials_binding->rekey_credential(
            admin_credentials_binding->context, current, current_length,
            password, password_length)) {
        console_admin_credentials_end();
        console_write(
            "[ADMIN-ENROLLMENT] credentials refused: "
            "HOST_SEED_REKEY_FAILED\n");
        console_admin_credentials_state();
        prompt();
        return;
    }
    status = admin_credentials_binding->provision(
        admin_credentials_binding->context, login, login_length, password,
        password_length, now_tick);
    console_admin_credentials_end();
    if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        console_write("[ADMIN-ENROLLMENT] credentials refused: ");
        console_write(console_admin_credentials_status_text(status));
        console_putc('\n');
    } else {
        /*
         * The record names what changed (the login) and never the password:
         * the login is not a secret and the operator has to be able to tell
         * which account the next sign-in uses.  The same line reaches the
         * durable journal, because a change of the one account is exactly the
         * kind of event the journal exists for.
         */
        static const char changed_prefix[] =
            "[ADMIN-ENROLLMENT] credentials changed login=";
        char changed_record[sizeof(changed_prefix) +
                            APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX];
        size_t changed_length = 0U;

        for (size_t index = 0U; index < sizeof(changed_prefix) - 1U; ++index) {
            changed_record[changed_length++] = changed_prefix[index];
        }
        for (size_t index = 0U; changed_login[index] != '\0'; ++index) {
            changed_record[changed_length++] = changed_login[index];
        }
        changed_record[changed_length] = '\0';
        console_write(changed_record);
        console_putc('\n');
        console_journal_record(changed_record, changed_length);
    }
    console_admin_credentials_state();
    prompt();
}

static void console_admin_credentials_login(void) {
    const char *login;
    const char *password;
    size_t login_length = 0U;
    size_t password_length = 0U;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    size_t index;
    const uint64_t now_tick = (uint64_t)hfoc_lwip_time_now();
    enum appliance_production_admin_enrollment_status status;

    login = appliance_admin_credentials_prompt_login(
        &admin_credentials_prompt, &login_length);
    password = appliance_admin_credentials_prompt_password(
        &admin_credentials_prompt, &password_length);
    if (login == NULL || password == NULL) {
        console_write("[ADMIN-ENROLLMENT] conversation lost\n");
        console_admin_credentials_end();
        prompt();
        return;
    }
    for (index = 0U; index < sizeof(token); ++index) {
        token[index] = 0U;
    }
    status = admin_credentials_binding->authenticate(
        admin_credentials_binding->context, login, login_length, password,
        password_length, now_tick, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
        token, &session_id);
    console_admin_credentials_end();
    if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        console_write("[ADMIN-ENROLLMENT] login refused: ");
        console_write(console_admin_credentials_status_text(status));
        console_putc('\n');
        console_admin_credentials_state();
        prompt();
        return;
    }
    console_write("ADMIN SESSION: Authorization: Bearer ");
    for (index = 0U; index < sizeof(token); ++index) {
        console_putc((char)token[index]);
    }
    console_putc('\n');
    for (index = 0U; index < sizeof(token); ++index) {
        token[index] = 0U;
    }
    /* ITEM-516: a successful local sign-in is the proof `gate open` needs. */
    console_gate_record_local_proof();
    console_admin_credentials_state();
    prompt();
}

static void console_admin_credentials_reset(void) {
    static const char reset_record[] =
        "[ADMIN-ENROLLMENT] reset; state=UNPROVISIONED";
    enum appliance_production_admin_enrollment_status status;

    if (admin_credentials_binding == NULL ||
        admin_credentials_binding->reset == NULL) {
        console_write("[ADMIN-ENROLLMENT] UNAVAILABLE\n");
        return;
    }
    status = admin_credentials_binding->reset(
        admin_credentials_binding->context);
    if (status != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK) {
        console_write("[ADMIN-ENROLLMENT] reset refused: ");
        console_write(console_admin_credentials_status_text(status));
        console_putc('\n');
        console_admin_credentials_state();
        return;
    }
#ifdef HFOC_PRODUCTION_TLS
    if (ssh_revoke_callback != NULL &&
        !ssh_revoke_callback(ssh_revoke_context)) {
        static const char warning[] =
            "[SSH-KEY] warning: the credential reset succeeded but the"
            " authorized-key revoke failed; the listener stays stopped and"
            " the old key may return after reboot";
        console_write(warning);
        console_putc('\n');
        console_journal_record(warning, sizeof(warning) - 1U);
    }
#endif
    console_write(reset_record);
    console_putc('\n');
    console_journal_record(reset_record, strlen(reset_record));
    console_admin_credentials_state();
}

/*
 * ITEM-610: the credential questions are answered on a line the operator can
 * read.  LOGIN_NAME/NEW_LOGIN show the accepted characters themselves; every
 * password step shows exactly one `*` per accepted character.  The echo is
 * framebuffer-only on purpose: the serial transcript, the durable journal and
 * the command history keep no copy of a credential answer, and a real password
 * byte is never painted.
 */
static bool console_admin_credentials_step_is_secret(void) {
    switch (admin_credentials_prompt.step) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD:
        return true;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN:
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME:
    default:
        return false;
    }
}

static void console_admin_credentials_echo_character(char value) {
    console_emit(console_admin_credentials_step_is_secret() ? '*' : value,
                 false);
}

/*
 * Backspace removes one accepted character's feedback, never the question.
 * The normal one-cell path is enough while the cursor is inside a row; at the
 * start of a wrapped row the previous cell is the last cell of the row above.
 */
static void console_admin_credentials_echo_backspace(void) {
    if (cursor_x > LEFT) {
        cursor_x -= console_cell_width;
        erase_cell();
        console_note_echo_paint();
        return;
    }
    if (cursor_y >= BODY_TOP + 8U + console_cell_height) {
        cursor_y -= console_cell_height;
        cursor_x = LEFT +
            ((framebuffer_width() - LEFT - console_cell_width) /
             console_cell_width) * console_cell_width;
        erase_cell();
        console_note_echo_paint();
    }
}

/*
 * ITEM-610: the operator gets an immediate, honest sign that ENTER started the
 * potentially long verification, and the frame that carries it is actually
 * presented before the synchronous work begins.  The text is prose from the
 * catalog; no percentage, success or secret is invented here.
 */
static void console_admin_credentials_checking(void) {
    console_write_atomic(
        console_local_menu_text("console.prose.credentials_checking"));
    console_flush_deferred();
    framebuffer_flush();
}

static void console_admin_credentials_enter(void) {
    char text[APPLIANCE_ADMIN_CREDENTIALS_PROMPT_TEXT_MAX];
    size_t text_length = 0U;
    enum appliance_admin_credentials_prompt_action action;

    if (input_length == 0U) {
        /* An empty line at any step is the way out; nothing is committed. */
        (void)appliance_admin_credentials_prompt_cancel(
            &admin_credentials_prompt, text, sizeof(text), &text_length);
        console_admin_credentials_zero_line();
        console_write_atomic(text);
        console_putc('\n');
        console_admin_credentials_end();
        prompt();
        return;
    }
    if (input_overflow) {
        /*
         * A line that did not fit is refused as too long, never cut to the
         * policy maximum: the module answers the over-long length without
         * reading the line, so the synthetic length is exactly the refusal.
         */
        action = appliance_admin_credentials_prompt_submit(
            &admin_credentials_prompt, input_line,
            APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX + 1U, text,
            sizeof(text), &text_length);
    } else {
        action = appliance_admin_credentials_prompt_submit(
            &admin_credentials_prompt, input_line, input_length, text,
            sizeof(text), &text_length);
    }
    console_admin_credentials_zero_line();
    switch (action) {
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_PROMPT:
        console_write_atomic(text);
        return;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_CHANGE:
        console_admin_credentials_checking();
        console_admin_credentials_commit();
        return;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_LOGIN:
        console_admin_credentials_checking();
        if (entropy_login_auto) {
            console_entropy_unlock_commit();
            return;
        }
        console_admin_credentials_login();
        return;
    case APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_CANCELLED:
        console_write_atomic(text);
        console_putc('\n');
        console_admin_credentials_end();
        prompt();
        return;
    default:
        /*
         * ITEM-509 increment 20: a missing translation ends the conversation
         * with the module's named refusal instead of a silent English message.
         */
        if (text_length != 0U) {
            console_write_atomic(text);
            console_putc('\n');
        } else {
            console_write("[ADMIN-ENROLLMENT] conversation refused\n");
        }
        console_admin_credentials_end();
        prompt();
        return;
    }
}

/*
 * ITEM-509 increment 7: the console renderer of the help catalogue.
 *
 * One content, two renderers: the four strings of a topic live in the one
 * language catalog, so help is translated and glyph-covered like every other
 * interface sentence, and the panel will render the same four strings in
 * increment 8 - the surfaces cannot tell different stories.  The strings are
 * authored as whole lines; the renderer splits them at their own newlines and
 * writes every line with the IRQ-safe sequence helpers, so an IRQ trace record
 * cannot land inside a heading a proof waits for.  A string the catalog does
 * not carry is a named refusal, never half an answer and never a silent
 * fallback to English.
 */
static bool console_help_append(char *line, size_t capacity, size_t *length,
                                const char *text) {
    if (line == NULL || length == NULL || text == NULL) {
        return false;
    }
    while (*text != '\0') {
        if (*length + 1U >= capacity) {
            return false;
        }
        line[(*length)++] = *text++;
    }
    return true;
}

static void console_help_write_text(const char *text) {
    char line[CONSOLE_HELP_LINE_MAX];
    size_t length = 0U;

    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        const unsigned char byte = (unsigned char)*text;

        if (byte == '\n') {
            line[length++] = '\n';
            line[length] = '\0';
            console_write_atomic(line);
            length = 0U;
        } else {
            /*
             * Break before a new code point when the line is nearly full; a
             * continuation byte never starts a line, so both halves stay valid
             * UTF-8 and no glyph turns into a marker.
             */
            if (length + 5U > sizeof(line) && (byte & 0xC0U) != 0x80U) {
                line[length++] = '\n';
                line[length] = '\0';
                console_write_atomic(line);
                length = 0U;
            }
            line[length++] = (char)byte;
        }
        ++text;
    }
    if (length != 0U) {
        line[length++] = '\n';
        line[length] = '\0';
        console_write_atomic(line);
    }
}

/*
 * The coverage proof is what keeps the catalog complete; if it is not, the
 * answer is a visible machine refusal - not a topic rendered half in English.
 */
static void console_help_unavailable(void) {
    static const char text[] = "HELP IS NOT AVAILABLE\n";
    static const char record[] =
        "[CONSOLE] help unavailable reason=CATALOG_INCOMPLETE";

    console_write_atomic(text);
    console_journal_record(record, sizeof(record) - 1U);
}

static bool console_help_topic_complete(size_t index) {
    size_t field;

    if (appliance_help_topic_id(index) == NULL) {
        return false;
    }
    for (field = 0U; field < (size_t)APPLIANCE_HELP_FIELD_COUNT; ++field) {
        if (appliance_help_topic_string(
                index, (enum appliance_help_field)field,
                console_language) == NULL) {
            return false;
        }
    }
    return true;
}

static void console_help_render_topic(size_t index) {
    static const enum appliance_help_field fields[] = {
        APPLIANCE_HELP_FIELD_TITLE,
        APPLIANCE_HELP_FIELD_SUMMARY,
        APPLIANCE_HELP_FIELD_BODY,
        APPLIANCE_HELP_FIELD_RELATED
    };
    size_t field;

    if (!console_help_topic_complete(index)) {
        console_help_unavailable();
        return;
    }
    for (field = 0U; field < sizeof(fields) / sizeof(fields[0]); ++field) {
        console_help_write_text(
            appliance_help_topic_string(index, fields[field],
                                        console_language));
    }
}

/* `install: <summary>` - the table-of-contents line of one topic. */
static void console_help_render_toc_line(size_t index) {
    char line[CONSOLE_HELP_LINE_MAX];
    const char *id = appliance_help_topic_id(index);
    const char *summary = appliance_help_topic_string(
        index, APPLIANCE_HELP_FIELD_SUMMARY, console_language);
    size_t length = 0U;

    if (id == NULL || summary == NULL ||
        !console_help_append(line, sizeof(line), &length, id) ||
        !console_help_append(line, sizeof(line), &length, ": ") ||
        !console_help_append(line, sizeof(line), &length, summary)) {
        console_help_unavailable();
        return;
    }
    line[length] = '\0';
    console_help_write_text(line);
}

static void console_help_command(enum console_help_command_kind kind,
                                 size_t topic) {
    const char *title;
    const char *topics;
    const char *usage;
    const char *missing;
    size_t index;

    if (kind == CONSOLE_HELP_COMMAND_REFUSE) {
        missing = appliance_language_string(console_language,
                                            "help.topic_missing");
        usage = appliance_language_string(console_language, "help.usage");
        if (missing == NULL || usage == NULL) {
            console_help_unavailable();
            return;
        }
        console_help_write_text(missing);
        console_help_write_text(usage);
        return;
    }
    if (kind == CONSOLE_HELP_COMMAND_TOPIC) {
        console_help_render_topic(topic);
        return;
    }
    if (kind == CONSOLE_HELP_COMMAND_ALL) {
        for (index = 0U; index < appliance_help_topic_count(); ++index) {
            if (index != 0U) {
                console_write_atomic("\n");
            }
            console_help_render_topic(index);
        }
        return;
    }
    /* LIST: the title, one line per topic, the usage, and the vocabulary line
       every operator and every proof already reads. */
    title = appliance_language_string(console_language, "help.title");
    topics = appliance_language_string(console_language, "help.topics");
    usage = appliance_language_string(console_language, "help.usage");
    if (title == NULL || topics == NULL || usage == NULL) {
        console_help_unavailable();
        return;
    }
    console_help_write_text(title);
    console_help_write_text(topics);
    for (index = 0U; index < appliance_help_topic_count(); ++index) {
        console_help_render_toc_line(index);
    }
    console_help_write_text(usage);
    /*
     * One literal and one IRQ-safe write: the ITEM-418 proof reads this
     * vocabulary line from the source and compares every console proof's
     * `help_marker` with it, so the line stays exactly as it was written.
     */
    {
        const uint64_t flags = serial_sequence_begin();

        console_write("COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD MEM PMM HEAP ABOUT APPLIANCE APPLIANCE-BOOT CREDENTIALS LOGIN LANGUAGE REBOOT SHUTDOWN PANIC DIVZERO PAGEFAULT DOUBLEFAULT VMMMAP BOOTLOG MENU XHCI INPUT DIAG PTR ARM\n");
        serial_sequence_end(flags);
    }
}

static void execute_command(void) {
    input_line[input_length] = '\0';
    /*
     * ITEM-506: the account command family takes no argument and a hidden
     * answer is not a command, so an ordinary submitted line is history-worthy.
     * The two removed spellings are the exception: a manual can still make an
     * operator type a password after them, and the history must not recall it.
     */
    if (input_length != 0U && !console_legacy_secret_command(input_line)) {
        history_add(input_line);
    }
    if (input_length == 0U) {
        /*
         * ITEM-430: the offer the machine painted is opened by the one key an
         * operator at a screen tries first.  Any other key cancels the offer
         * (handled where the key arrives), so typing is never hijacked: an
         * empty line and ENTER is the whole gesture, and a line with anything
         * on it is a command as before.
         */
        if (menu_offer_open) {
            menu_offer_open = false;
            cmd_local_menu();
            reset_history_navigation_state();
            reset_input_state();
            return;
        }
        prompt();
        reset_history_navigation_state();
        reset_input_state();
        return;
    }
    /*
     * ITEM-460: from here to the closing `console_transcript_end()` the line
     * just entered and every character the command writes are the transcript.
     * The command is journaled before it runs, so a command whose answer never
     * arrives (a panic, a power action) is still on the record.
     */
    console_transcript_command(input_line);
    console_transcript_begin();
    /*
     * ITEM-506: the one account vocabulary is classified before the command
     * chain, and on every submitted line, so a line that is not the reset
     * confirmation disarms the pending request.  `credentials` and `login`
     * start conversations here; a password never arrives as an argument.
     */
    enum console_credentials_command_kind credentials_kind =
        CONSOLE_CREDENTIALS_COMMAND_NONE;
    /*
     * ITEM-509 increment 4: the language vocabulary is classified before the
     * command chain as well, and on the whole line, so `language de` reaches
     * the named refusal instead of the generic unknown-command answer.
     */
    enum console_language_command_kind language_kind =
        CONSOLE_LANGUAGE_COMMAND_NONE;
    enum appliance_language language_request = APPLIANCE_LANGUAGE_DEFAULT;
    const char *language_value = NULL;
    size_t language_value_length = 0U;
    /*
     * ITEM-509 increment 7: the help vocabulary is classified before the
     * command chain too, so `help install` reaches the topic renderer and
     * `help nonsense` reaches the named refusal instead of the generic
     * unknown-command answer.
     */
    enum console_help_command_kind help_kind = CONSOLE_HELP_COMMAND_NONE;
    size_t help_topic = (size_t)-1;
    /*
     * ITEM-516: the management-gate vocabulary is classified before the
     * command chain as well, and on every submitted line, so `gate open`
     * reaches the warning, `gate opne` reaches the named refusal, and any
     * other line disarms an armed confirmation.
     */
    enum console_gate_command_kind gate_kind = CONSOLE_GATE_COMMAND_NONE;
#ifdef HFOC_PRODUCTION_TLS
    enum console_ssh_key_command_kind ssh_key_kind =
        CONSOLE_SSH_KEY_COMMAND_NONE;
    const char *ssh_key_argument = NULL;
    size_t ssh_key_argument_length = 0U;
#endif

    credentials_kind = console_credentials_command_classify(
        input_line, input_length, &credentials_reset_pending);
    language_kind = console_language_command_classify(
        input_line, input_length, &language_request, &language_value,
        &language_value_length);
    help_kind = console_help_command_classify(input_line, input_length,
                                              &help_topic);
    gate_kind = console_gate_command_classify(input_line, input_length,
                                              &console_gate_open_armed);
#ifdef HFOC_PRODUCTION_TLS
    ssh_key_kind = console_ssh_key_classify(
        input_line, &ssh_key_argument, &ssh_key_argument_length);
    if (ssh_key_kind == CONSOLE_SSH_KEY_COMMAND_NONE) {
        ssh_key_pending = false;
    }
    if (ssh_key_kind != CONSOLE_SSH_KEY_COMMAND_NONE) {
        console_ssh_key_command(ssh_key_kind, ssh_key_argument,
                                ssh_key_argument_length);
    } else
#endif
    if (credentials_kind == CONSOLE_CREDENTIALS_COMMAND_CHANGE) {
        console_admin_credentials_begin(
            APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE);
    } else if (credentials_kind == CONSOLE_CREDENTIALS_COMMAND_LOGIN) {
        console_admin_credentials_begin(
            APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_LOGIN);
    } else if (credentials_kind == CONSOLE_CREDENTIALS_COMMAND_RESET_ARM) {
        static const char arm_record[] =
            "[ADMIN-ENROLLMENT] reset requires confirmation; type "
            "`CREDENTIALS RESET CONFIRM` on the next line";

        console_write(arm_record);
        console_putc('\n');
        console_journal_record(arm_record, strlen(arm_record));
    } else if (credentials_kind ==
               CONSOLE_CREDENTIALS_COMMAND_RESET_EXECUTE) {
        console_admin_credentials_reset();
    } else if (credentials_kind ==
               CONSOLE_CREDENTIALS_COMMAND_RESET_UNCONFIRMED) {
        static const char refused_record[] =
            "[ADMIN-ENROLLMENT] reset refused: no CREDENTIALS RESET was "
            "requested";

        console_write(refused_record);
        console_putc('\n');
        console_journal_record(refused_record, strlen(refused_record));
    } else if (language_kind != CONSOLE_LANGUAGE_COMMAND_NONE) {
        console_language_command(language_kind, language_request,
                                 language_value, language_value_length);
    } else if (gate_kind != CONSOLE_GATE_COMMAND_NONE) {
        console_gate_command(gate_kind);
    } else if (help_kind != CONSOLE_HELP_COMMAND_NONE) {
        console_help_command(help_kind, help_topic);
#ifndef HFOC_CONSOLE_HOST_TEST
    } else if (console_command_is(input_line, "diag save")) {
        /*
         * ITEM-552: the operator's one action.  The report is composed from
         * the machine's own answers, written into the journal the medium's
         * build reserved, flushed, read back and compared - so SAVED is only
         * ever printed for a session the other machine's extractor can read,
         * and a refusal names the step that stopped it.
         */
        struct diag_export_session session;
        enum diag_export_result result;

        /* ITEM-582: the installed system's periodic console records must not
           land inside the export's session while it is written and read back;
           the funnel is suspended for the duration and restored immediately. */
        install_journal_platform_suspend_observe(true);
        result = diag_export_save(&session);
        install_journal_platform_suspend_observe(false);

        if (result == DIAG_EXPORT_OK) {
            static const char digits[] = "0123456789ABCDEF";
            char identifier[9];

            for (uint32_t index = 0U; index < 8U; ++index) {
                identifier[index] =
                    digits[(session.id >> (28U - index * 4U)) & 0x0FU];
            }
            identifier[8] = '\0';
            console_write("DIAG SAVED id=");
            console_write(identifier);
            console_write(" records=");
            write_u64(session.records);
            console_write(" bytes=");
            write_u64(session.bytes);
            console_putc('\n');
        } else {
            /*
             * ITEM-557: two lines, in the two sizes the operator needs.  The
             * first fits one row and carries the step and the cause, so the
             * answer survives a screen that scrolls or a line that wraps; the
             * second carries the whole detail.  The same refusal goes into the
             * boot log as `[DIAG-FAIL]`, so a machine whose journal medium
             * cannot be written still carries the reason off the screen, and
             * `diag` prints both lines again on demand - the next attempt's
             * reset can no longer erase the failure before it is read.
             */
            const struct diag_failure *failure =
                diag_export_last_failure();
            char short_line[DIAG_FAILURE_ROW_MAX + 1U];

            if (diag_failure_short_line(failure, short_line,
                                        sizeof(short_line)) != 0U) {
                console_write(short_line);
                console_putc('\n');
            }
            console_write("DIAG DETAIL ");
            console_write(failure->detail);
            console_putc('\n');
            diag_failure_record(failure);
            print_diag_snapshot();
        }
    } else if (console_command_is(input_line, "diag")) {
        const struct diag_failure *failure =
            diag_export_last_failure();
        char short_line[DIAG_FAILURE_ROW_MAX + 1U];

        if (!failure->recorded ||
            diag_failure_short_line(failure, short_line,
                                    sizeof(short_line)) == 0U) {
            console_write("DIAG: no failure recorded this boot\n");
        } else {
            /* The recorded refusal under its own prefix: same fields as the
               line `diag save` printed, under a name that says this is the
               answer to the question just asked. */
            (void)diag_failure_short_line_prefixed(
                failure, "DIAG LAST", short_line, sizeof(short_line));
            console_write(short_line);
            console_putc('\n');
            console_write("DIAG DETAIL ");
            console_write(failure->detail);
            console_putc('\n');
        }
        if (diag_export_snapshot_records() != 0U) {
            console_write("DIAG SNAPSHOT: diag snapshot prints it again\n");
        }
    } else if (console_command_is(input_line, "diag snapshot")) {
        if (diag_export_snapshot_records() == 0U) {
            console_write("DIAG SNAPSHOT: none this boot\n");
        } else {
            print_diag_snapshot();
        }
#ifndef HFOC_CONSOLE_HOST_TEST
    } else if (console_command_is(input_line, "ehci")) {
        /*
         * ITEM-562: the USB 2 controllers' own verdicts on demand.  The boot
         * records `[EHCI]`/`[EHCI-PORT]` are journal records, and the journal
         * is exactly what is unbound on the machine that has no boot medium -
         * so the walk's evidence disappeared with it, and `bootlog` answered
         * "unavailable".  The command prints the same lines the diagnostic
         * report carries, from RAM, for every controller separately: which
         * port's path each walk ended at, how many devices it offered the
         * medium guard, and the start words of each controller.
         */
        ehci_diagnostics_print();
#endif
    } else if (console_command_is(input_line, "xhci")) {
        /*
         * ITEM-468 (СРОЧНО-18): the controllers' live state on demand, so a
         * machine with a silent pointer can be read where it stands instead of
         * after the journal has been carried to another computer.  The answer
         * goes through the console transcript (ITEM-460), so it reaches the
         * durable journal as well.  The driver owns the formatting, so the
         * command is absent from the host-only console build exactly like
         * `reboot` and `admin` above.
         */
        xhci_diagnostics_print_xhci();
    } else if (console_command_is(input_line, "input")) {
        /* ITEM-468: the HID roles' binding and traffic state, and the window
           the loop last closed, on demand. */
        xhci_diagnostics_print_input();
#endif
    } else if (console_command_is(input_line, "ptr") ||
               console_command_prefix(input_line, "ptr ")) {
        /* ITEM-472: show or set the operator's pointer gain. */
        console_pointer_command(input_line);
    } else if (console_command_is(input_line, "arm") ||
               console_command_prefix(input_line, "arm ")) {
        /* ITEM-473: show or set the mouse's armed-transfer depth. */
        console_arm_command(input_line);
    } else if (console_command_is(input_line, "menu")) {
        cmd_local_menu();
    } else if (console_command_is(input_line, "scroll")) {
        scroll_command();
    } else if (console_command_is(input_line, "clear")) {
        clear_body();
    } else if (console_command_is(input_line, "version")) {
        /*
         * ITEM-558: the loaded kernel's revision, where the operator already
         * asks what the machine is.  Before this, the build identity reached
         * the screen only through a saved report's BEGIN record, and this
         * machine could not bind a journal at all.
         */
        console_write("HFOC OS 1.0.0 X86-64 build=");
        console_write(hfoc_build_id());
        console_write(" version=");
        console_write(hfoc_version_string());
        console_putc('\n');
    } else if (console_command_is(input_line, "uptime")) {
        console_write(console_label_text("console.label.uptime"));
        write_u64(timer_uptime_seconds());
        console_write(" SECONDS\n");
    } else if (console_command_is(input_line, "ticks")) {
        console_write(console_label_text("console.label.ticks"));
        write_u64(timer_ticks());
        console_putc('\n');
    } else if (console_command_is(input_line, "irq")) {
        console_write(console_label_text("console.label.if"));
        console_write((cpu_read_rflags() & (1ULL << 9U)) != 0U ? "ON" : "OFF");
        console_write(console_label_text("console.label.pic_master_mask"));
        write_u64(pic_master_mask());
        console_write(console_label_text("console.label.pic_slave_mask"));
        write_u64(pic_slave_mask());
        console_write(console_label_text("console.label.timer_irq"));
        write_u64(timer_ticks());
        console_write(console_label_text("console.label.irq_keyboard"));
        write_u64(keyboard_irq_count());
        console_putc('\n');
    } else if (console_command_is(input_line, "kbd")) {
        /*
         * ITEM-395: the local input path is published next to the PS/2
         * counters, so an operator on a machine without PS/2 reads which
         * keyboard is live (and the USB report count proves it is being
         * serviced) instead of an IRQ count that can only stay zero.
         */
        char input_line_buffer[LOCAL_INPUT_RENDER_MAX];

        console_write(console_label_text("console.label.keyboard_irq"));
        write_u64(keyboard_irq_count());
        console_write(console_label_text("console.label.last_scancode"));
        write_u64(keyboard_last_scancode());
        console_putc('\n');
        (void)local_input_render(input_line_buffer,
                                 sizeof(input_line_buffer));
        console_write(input_line_buffer);
        /*
         * ITEM-395: the live USB counters, next to the PS/2 counters above.
         * They are read from the driver rather than from the boot-time
         * publication, so a typing operator sees the report count move.
         *
         * ITEM-436: `REFUSED` and `LAST` are the two numbers that separate a
         * keyboard which answers nothing from one whose answers the decoder
         * never sees - a completed transfer that carried fewer than the eight
         * bytes of a boot report is refused and counted, and LAST is the byte
         * count of the most recent completion.
         *
         * ITEM-438: KEYS is the characters those reports produced, so a
         * keyboard that answers every poll with an idle report reads as
         * `REPORTS>0 KEYS=0` instead of as a working keyboard.
         */
        console_write("USB HID: REPORTS=");
        write_u64(xhci_keyboard_report_count());
        console_write(" TRANSFERS=");
        write_u64(xhci_keyboard_transfer_count());
        console_write(" ERRORS=");
        write_u64(xhci_keyboard_error_count());
        console_write(" REFUSED=");
        write_u64(xhci_keyboard_refused_count());
        console_write(" LAST=");
        write_u64(xhci_keyboard_last_report_bytes());
        /*
         * ITEM-461: KEYS stays where ITEM-438 and ITEM-458 pinned it; the
         * re-arm wait is on the `[PRODUCTION-LOOP]` heartbeat (and in the
         * journal), where it does not change a line other items match.
         */
        console_write(" KEYS=");
        write_u64(xhci_keyboard_key_count());
        /*
         * ITEM-458: the two numbers behind "no character is lost".
         * `DROPPED` counts reports the bounded console queue refused, and
         * `GAP_MS_MAX` the longest wait between two reports the service pass
         * delivered - a bound report that waited shows up here instead of
         * only as a character that never arrived.
         */
        console_write(" DROPPED=");
        write_u64(xhci_keyboard_dropped_count());
        console_write(" GAP_MS_MAX=");
        write_u64(xhci_keyboard_gap_ms_max());
        console_putc('\n');
        /*
         * ITEM-461: the mode and the two costs on demand, so a proof that has
         * just driven the console under load reads the numbers of that load.
         * The same numbers are published as `[FB]`, `[CONSOLE] paint` and
         * `[PRODUCTION-LOOP]` records in the durable journal.
         */
        console_write("[FB] width=");
        write_u64(display_width);
        console_write(" height=");
        write_u64(display_height);
        console_write(" pitch=");
        write_u64(display_pitch);
        console_write(" cells=");
        write_u64(display_width / console_cell_width);
        console_write("x");
        write_u64(display_height / console_cell_height);
        console_write(" bpp=");
        write_u64(display_bpp);
        console_write(" requested=");
        write_u64(display_requested_width);
        console_write("x");
        write_u64(display_requested_height);
        console_write(" actual=");
        write_u64(display_width);
        console_write("x");
        write_u64(display_height);
        console_write(" fallback=");
        write_u64(display_fallback);
        /* ITEM-466: the same tail the boot record carries, so a proof that
           reads the `kbd` command's line sees the scale and the preference. */
        console_write(" scale=");
        write_u64(console_scale);
        console_write(" cell_px=");
        write_u64(console_cell_width);
        console_write("x");
        write_u64(console_cell_height);
        console_write(" preferred=");
        write_u64(display_requested_width);
        console_write("x");
        write_u64(display_requested_height);
        {
            char available[160];

            available[0] = '\0';
            (void)framebuffer_available_text(available, sizeof(available));
            console_write(" available_count=");
            write_u64(display_available_count);
            console_write(" available=");
            console_write(available);
        }
        console_putc('\n');
        console_write("[CONSOLE] paint lines=");
        write_u64(console_render_line_count());
        console_write(" chars=");
        write_u64(console_render_char_count());
        console_write(" us=");
        write_u64(console_paint_average_us());
        console_write(" max_us=");
        write_u64(console_render_max_us());
        console_putc('\n');
        /* ITEM-466: how many records the screen budget kept off the screen,
           while the durable journal kept every one of them. */
        console_write("[CONSOLE] screen suppressed=");
        write_u64(console_render_screen_suppressed());
        console_putc('\n');
        console_write("[PRODUCTION-LOOP] iter_us=");
        write_u64(kernel_loop_iter_us());
        console_write(" iter_max_us=");
        write_u64(kernel_loop_iter_max_us());
        console_putc('\n');
    } else if (console_command_is(input_line, "mem")) {
        console_write(console_label_text("console.label.usable_memory"));
        write_u64(usable_memory_mib);
        console_write(" MIB\n");
    } else if (console_command_is(input_line, "pmm")) {
        const struct pmm_init_diagnostic *pmm = pmm_init_diagnostic();
        console_write("[PMM ] usable=");
        write_u64(pmm_total_usable_bytes());
        console_write(" free_pages=");
        write_u64(pmm_free_page_count());
        /*
         * ITEM-390: the operator sees whether the bitmap covers all usable
         * memory or dropped part of it, and how many pages it tracks.
         */
        console_write(" pages=");
        write_u64(pmm->total_pages);
        console_write(" cap=");
        write_u64(pmm->page_cap);
        console_write(" coverage=");
        console_write(pmm_coverage_string(pmm->coverage));
        console_write(" dropped_pages=");
        write_u64(pmm->dropped_pages);
        console_putc('\n');
    } else if (console_command_is(input_line, "heap")) {
        console_write("[HEAP] ready=");
        console_write(heap_ready() ? "yes" : "no");
        console_write(" capacity=");
        write_u64(heap_capacity_bytes());
        console_write(" used=");
        write_u64(heap_used_bytes());
        console_putc('\n');
    } else if (console_command_is(input_line, "about")) {
        console_write("GDT IDT TSS/IST CPU EXCEPTIONS LEGACY PIC PIT IRQ PS2 KEYBOARD FRAMEBUFFER CONSOLE\n");
    } else if (console_command_is(input_line, "appliance")) {
        cmd_appliance();
    } else if (console_command_is(input_line, "entropy") ||
               console_command_prefix(input_line, "entropy ")) {
        /*
         * ITEM-585: the explicit fallback for a CPU without RDRAND/RDSEED.
         * The command takes no argument; the seed itself is typed on the
         * hidden line so it never enters the command history or transcript.
         */
        if (console_command_is(input_line, "entropy") ||
            console_command_is(input_line, "entropy unlock")) {
            console_entropy_unlock_begin();
        } else if (console_command_is(input_line, "entropy seed")) {
            console_entropy_seed_begin();
        } else if (console_command_prefix(input_line, "entropy seed ")) {
            console_write("[RNG] type `entropy seed` with no argument;"
                          " the"
                          " seed is entered on a hidden line\n");
        } else if (console_command_prefix(input_line, "entropy ") &&
                   console_command_prefix(input_line, "entropy unlock") &&
                   input_line[14] == ' ') {
            console_write("[RNG] type `entropy unlock` with no argument;"
                          " login and password are entered on hidden lines\n");
        } else {
            console_write("[RNG] usage: entropy unlock | entropy seed\n");
        }
#ifndef HFOC_CONSOLE_HOST_TEST
    } else if (console_command_is(input_line, "network")) {
        /*
         * ITEM-581: the current network and panel readiness on demand.  The
         * installed Toshiba had no address and no panel and no command that
         * answered either; this prints the driver's view, the addressing
         * state, the DHCP exchange counters, the serving gate and the entropy
         * stage even when the network never came up.
         */
        kernel_production_status_report(console_status_report_sink, NULL);
#endif
    } else if (console_command_is(input_line, "bootlog")) {
        /*
         * ITEM-410: the log as this boot published it, read from the log's
         * bounded RAM ring.  The command deliberately does not read the disk:
         * the question it answers is "what did this machine say", and it has to
         * answer it on a machine whose log could not be written at all.
         */
        if (boot_log_bound) {
            boot_log_binding.dump();
        } else {
            console_write(
                console_local_menu_text("console.prose.bootlog_unavailable"));
        }
#ifndef HFOC_CONSOLE_HOST_TEST
    } else if (console_command_is(input_line, "reboot") ||
               console_command_is(input_line, "shutdown")) {
        /*
         * ITEM-418: the two actions an operator needs from the machine itself.
         * The module publishes what it is about to do before it does it, so the
         * record is on the serial stream and on the screen whether or not the
         * platform then obeys: a machine still running after an S5 write says
         * so instead of leaving the operator guessing.
         */
        const enum platform_power_action power_action =
            input_line[0U] == 'r' ? PLATFORM_POWER_ACTION_REBOOT
                                  : PLATFORM_POWER_ACTION_SHUTDOWN;
        struct platform_power_plan power_plan;

        if (platform_power_plan_current(power_action, &power_plan) !=
            PLATFORM_POWER_OK) {
            console_power_refusal(
                platform_power_action_string(power_action));
        } else {
            console_write(console_label_text("console.label.power"));
            console_write(platform_power_action_string(power_action));
            console_write(" via ");
            console_write(
                platform_power_mechanism_string(power_plan.mechanism));
            console_putc('\n');
            platform_power_execute(&power_plan);
        }
#endif
    } else if (console_command_is(input_line, "appliance-boot")) {
        cmd_appliance_boot();
    } else if (console_command_is(input_line, "storage")) {
        cmd_storage();
    } else if (console_command_prefix(input_line, "storage write-proof")) {
        cmd_storage_write_proof();
    } else if (console_command_is(input_line, "storage medium")) {
        cmd_storage_medium();
    } else if (console_command_is(input_line, "panic")) {
        kernel_panic("Panic requested from console");
    } else if (console_command_is(input_line, "divzero")) {
        console_write("TRIGGERING DIVIDE ERROR...\n");
        __asm__ volatile ("xor %%rdx, %%rdx; mov $1, %%rax; xor %%rcx, %%rcx; divq %%rcx" : : : "rax", "rcx", "rdx");
    } else if (console_command_is(input_line, "pagefault")) {
        volatile uint64_t *const address = (volatile uint64_t *)(uintptr_t)PAGE_FAULT_ADDRESS;
        console_write("TRIGGERING PAGE FAULT...\n");
        *address = 0U;
    } else if (console_command_is(input_line, "doublefault")) {
        cmd_doublefault();
    } else if (console_command_is(input_line, "vmmmap")) {
        volatile void *mmio = NULL;
        uint64_t hhdm = vmm_hhdm_offset();
        enum vmm_status s = vmm_map_mmio_page(0xFEE00000ULL, hhdm, &mmio);
        serial_write("[VMM] Mapping LAPIC page phys=0x00000000FEE00000\n");
        if (s == VMM_OK) {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[VMM] virtual=");
                serial_write_hex((uint64_t)(uintptr_t)mmio);
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }

            uint64_t pte = vmm_read_pte_diagnostic(0xFEE00000ULL, hhdm);
            if (pte != 0) {
                /* Verify PTE correctness: phys must match, flags correct */
                uint64_t pte_phys = pte & 0x000FFFFFFFFFF000ULL;
                if (pte_phys == 0xFEE00000ULL
                    && (pte & 1) && (pte & 2) && (pte & 8) && (pte & 16)
                    && !(pte & (1ULL << 7))) {
                    {
                        const uint64_t serial_flags = serial_sequence_begin();
                        serial_write("[VMM] PTE=");
                        serial_write_hex(pte);
                        serial_write(" P=1 RW=1 PWT=1 PCD=1 PAT=0\n");
                        serial_sequence_end(serial_flags);
                    }
                    serial_write("[VMM] MMIO page mapped successfully\n");
                } else {
                    serial_write("[VMM] map failed: PTE read-back mismatch\n");
                }
            } else {
                serial_write("[VMM] map failed: PTE read-back error\n");
            }
        } else {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[VMM] map failed: ");
                serial_write(vmm_status_string(s));
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
        }
    } else {
        uint64_t flags;
        /*
         * ITEM-460/ITEM-506: the refusal names the command.  An ordinary line
         * is echoed whole; the two removed spellings are redacted, because the
         * stale manual that produced them put a password after the name.
         */
        const char *echoed = console_legacy_secret_redact(input_line);

        console_serial_lock(&flags);
        console_write(console_label_text("console.label.unknown_command"));
        console_write(echoed);
        console_write(
            console_local_menu_text("console.prose.unknown_command_hint"));
        console_serial_unlock(flags);
    }
    console_transcript_end();
    if (pager_active) {
        reset_history_navigation_state();
        reset_input_state();
        return;
    }
    prompt();
    reset_history_navigation_state();
    reset_input_state();
}

void console_init(uint64_t usable_mib, uint32_t cpu_cores) {
    usable_memory_mib = usable_mib;
    boot_cpu_cores = cpu_cores;
    reset_history_state();
    reset_input_state();
    /*
     * ITEM-466: the mode decides the glyph scale, and the header follows it so
     * a 4K screen does not keep a two-line header next to giant body text.  A
     * smaller mode gets a smaller header rather than text the monitor then
     * stretches.
     */
    console_scale = framebuffer_font_scale(framebuffer_width(),
                                           framebuffer_height());
    console_cell_width = FRAMEBUFFER_FONT_CELL_WIDTH * console_scale;
    console_cell_height = FRAMEBUFFER_FONT_CELL_HEIGHT * console_scale;
    {
        const uint32_t header_scale = console_scale * 2U;
        const uint32_t subtitle_scale =
            console_scale >= 2U ? console_scale / 2U : 1U;

        framebuffer_clear(BACKGROUND);
        framebuffer_rect(0, 0, framebuffer_width(), 64, HEADER);
        framebuffer_text(18, 12, hfoc_version_title(), 0xffffff, header_scale);
        framebuffer_text(18, 66, "LOCAL CONSOLE", MUTED, subtitle_scale);
    }
    cursor_x = LEFT;
    cursor_y = BODY_TOP + 8U;
    /*
     * ITEM-478 (round 16): the only line the machine paints on its own is the
     * product banner; the readiness facts below reach the serial stream and the
     * durable journal through `console_service_line()`, so a screen with
     * nothing to say stays empty until the operator types or the menu is
     * offered.
     */
    console_write("HFOC OS 1.0.0 INITIALISED\n");
    /*
     * ITEM-392: readiness never implies a healthy console it does not have.
     * Without a keyboard the local console is a display only, so it says so
     * and names the paths that still reach the appliance.
     *
     * ITEM-395: a console driven by the USB HID keyboard says so instead of
     * claiming a PS/2 controller this machine does not have.
     */
    if (local_input_available && input_source == LOCAL_INPUT_SOURCE_PS2) {
        console_service_line("GDT IDT LEGACY PIC PIT PS2: OK\n",
                             strlen("GDT IDT LEGACY PIC PIT PS2: OK\n"));
        console_service_line(
            "TYPE HELP FOR COMMANDS, MENU FOR THE LOCAL MENU\n",
            strlen("TYPE HELP FOR COMMANDS, MENU FOR THE LOCAL MENU\n"));
    } else if (local_input_available) {
        console_service_line("GDT IDT LEGACY PIC PIT PS2: UNAVAILABLE\n",
                             strlen("GDT IDT LEGACY PIC PIT PS2: "
                                    "UNAVAILABLE\n"));
        console_service_line("LOCAL INPUT: ENABLED (USB HID KEYBOARD)\n",
                             strlen("LOCAL INPUT: ENABLED (USB HID "
                                    "KEYBOARD)\n"));
        console_service_line(
            "TYPE HELP FOR COMMANDS, MENU FOR THE LOCAL MENU\n",
            strlen("TYPE HELP FOR COMMANDS, MENU FOR THE LOCAL MENU\n"));
    } else {
        /*
         * ITEM-430: a machine that cannot type is shown the menu itself, so
         * the banner promises that instead of naming a command nobody here can
         * enter.  ITEM-478 keeps only that promise on the screen - it is the
         * menu's own offer - while the readiness facts go to the journal.
         */
        console_service_line("GDT IDT LEGACY PIC PIT PS2: UNAVAILABLE\n",
                             strlen("GDT IDT LEGACY PIC PIT PS2: "
                                    "UNAVAILABLE\n"));
        console_service_line("LOCAL INPUT: DISABLED (NO PS/2 KEYBOARD)\n",
                             strlen("LOCAL INPUT: DISABLED (NO PS/2 "
                                    "KEYBOARD)\n"));
        console_service_line("USE SERIAL OR THE MANAGEMENT PLANE\n",
                             strlen("USE SERIAL OR THE MANAGEMENT PLANE\n"));
        console_write_atomic(
            console_local_menu_text("console.menu.offered_reading"));
    }
    console_service_line("DOUBLE FAULT IST ENABLED\n",
                         strlen("DOUBLE FAULT IST ENABLED\n"));
    /*
     * ITEM-405: records published before console_ready was set are painted
     * here, once, after the banner and before the first prompt.  ITEM-478 sent
     * every service record to the journal, so this flushes only a record a
     * future item deliberately paints before the banner.
     */
    console_ready = true;
    console_record_flush();
    /*
     * ITEM-478 (round 16): name the boot screen's own paint count before the
     * operator's first keystroke can add to it.  A service record that painted
     * would push `lines` past the banner's one; `service_lines` proves the
     * markers still reached the journal while staying off the screen.
     */
    {
        char boot_record[96];
        size_t length = 0U;

        length = console_offer_record_append(
            boot_record, sizeof(boot_record), length,
            "[CONSOLE] boot paint lines=");
        length = console_scroll_append_u64(boot_record, sizeof(boot_record),
                                           length, console_render_line_count());
        length = console_offer_record_append(boot_record, sizeof(boot_record),
                                             length, " service_lines=");
        length = console_scroll_append_u64(boot_record, sizeof(boot_record),
                                           length, console_service_lines);
        length = console_offer_record_append(boot_record, sizeof(boot_record),
                                             length, "\n");
        console_service_line(boot_record, length);
    }
    prompt();
}

void console_publish_local_input(bool available) {
    local_input_available = available;
}

void console_publish_input_source(enum local_input_source source) {
    input_source = source;
}

static bool console_management_copy(
    char *destination,
    size_t capacity,
    const char *source)
{
    size_t index = 0U;

    if (destination == NULL || capacity == 0U || source == NULL) {
        return false;
    }
    while (source[index] != '\0') {
        if (index + 1U >= capacity) {
            return false;
        }
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return true;
}

/*
 * Paint a held notice once.  An unchanged URL is not repainted, and the
 * message always starts on a line of its own so it cannot be read as part of
 * the prompt or of a password echo.  Called only when no hidden conversation
 * owns the line.
 */
static void console_management_flush(void) {
    const size_t url_length = strlen(management_pending_url);
    bool same = false;

    if (!management_pending) {
        return;
    }
    management_pending = false;
    if (management_published && strlen(management_url) == url_length &&
        memcmp(management_url, management_pending_url, url_length) == 0) {
        same = true;
    }
    if (same) {
        return;
    }
    for (size_t index = 0U; index <= url_length; ++index) {
        management_url[index] = management_pending_url[index];
    }
    management_published = true;
    if (cursor_x != LEFT) {
        console_putc('\n');
    }
    console_write(console_label_text("console.label.management"));
    console_write(management_pending_url);
    console_write(" reachability=");
    console_write(management_pending_reachability);
    console_write(" cert=");
    console_write(management_pending_certificate);
    console_write("\n");
}

void console_publish_management(
    const char *url,
    size_t url_length,
    const char *reachability,
    const char *certificate)
{
    static const char prefix[] = "https://";

    if (url == NULL || url_length <= sizeof(prefix) - 1U ||
        url_length > APPLIANCE_BOOT_CONSOLE_URL_MAX ||
        reachability == NULL || certificate == NULL ||
        memcmp(url, prefix, sizeof(prefix) - 1U) != 0) {
        return;
    }
    if (url_length >= sizeof(management_pending_url)) {
        return;
    }
    {
        char next_reachability[CONSOLE_MANAGEMENT_STATE_MAX + 1U];
        char next_certificate[CONSOLE_MANAGEMENT_STATE_MAX + 1U];

        /*
         * Build the whole notice before replacing the held one: an over-long
         * state string must leave an older pending notice intact rather than
         * pair a new URL with the previous state.
         */
        if (!console_management_copy(
                next_reachability, sizeof(next_reachability), reachability) ||
            !console_management_copy(
                next_certificate, sizeof(next_certificate), certificate)) {
            return;
        }
        for (size_t index = 0U; index <= url_length; ++index) {
            management_pending_url[index] = url[index];
        }
        for (size_t index = 0U; index < sizeof(next_reachability); ++index) {
            management_pending_reachability[index] = next_reachability[index];
        }
        for (size_t index = 0U; index < sizeof(next_certificate); ++index) {
            management_pending_certificate[index] = next_certificate[index];
        }
    }
    management_pending = true;
    /*
     * A credential question (including the hidden operator-seed line) owns the
     * cursor and the echo.  The newest address waits for the prompt boundary;
     * any other time it is painted at once, exactly as before.
     */
    if (admin_credentials_active || entropy_seed_active) {
        return;
    }
    console_management_flush();
}
/*
 * ITEM-405: accept one record for the console mirror.
 *
 * The text is reduced to the one bounded line the console paints: everything
 * before the first newline (a record that carries a whole block paints its
 * first, diagnostic line), cut at CONSOLE_RECORD_MAX and closed with "...".
 * A record carrying a control character or a byte the console font cannot
 * render is refused instead of being painted as garbage, and an empty record
 * paints nothing.
 */
static bool console_record_accept(
    const char *record,
    size_t length,
    char line[CONSOLE_RECORD_LINE_MAX + 1U])
{
    size_t index = 0U;

    if (record == NULL || length == 0U) {
        return false;
    }
    while (index < length && index < CONSOLE_RECORD_MAX &&
           record[index] != '\n' && record[index] != '\r') {
        const unsigned char value = (unsigned char)record[index];

        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
        line[index] = (char)value;
        ++index;
    }
    if (index == 0U) {
        return false;
    }
    if (index == CONSOLE_RECORD_MAX && index < length &&
        record[index] != '\n' && record[index] != '\r') {
        line[index++] = '.';
        line[index++] = '.';
        line[index++] = '.';
    }
    line[index] = '\0';
    return true;
}

/*
 * ITEM-460: one record for the durable journal, and only the journal.
 *
 * The path deliberately does not go through `console_publish_record()`: that
 * funnel paints the framebuffer, and the transcript exists because the screen
 * is the wrong place for it.  The line is sanitised by the same
 * `console_record_accept()` the painted mirror uses - one bounded printable
 * ASCII line - so what the log stores is what a reader can read, whether or not
 * a screen ever showed it.
 */
static void console_journal_record(const char *text, size_t length) {
    char line[CONSOLE_RECORD_LINE_MAX + 1U];

    if (!boot_log_bound || !console_record_accept(text, length, line)) {
        return;
    }
    boot_log_binding.observe(line, strlen(line));
}

/*
 * ITEM-481: how many `-TAIL` continuations the publication funnel has produced.
 * The record itself is assembled whole and published once; the funnel is where
 * the CONSOLE_RECORD_MAX bound is enforced.
 */
static size_t console_record_split_parts;

/*
 * ITEM-481: publish a pre-built journal record under the same by-construction
 * bound.  An assembled record already arrives split, but a caller that hands
 * the funnel its own buffer - the console transcript, a service line - must not
 * have its tail cut off either: a line longer than CONSOLE_RECORD_MAX is
 * published as its own name plus as many `[<name>-TAIL]` continuations as it
 * needs, every one of them at or below the bound, with no byte dropped.  The
 * first line of a multi-block record is published, exactly as the console's own
 * sanitiser has always done.
 */
void console_publish_journal(const char *record, size_t length) {
    char buffer[CONSOLE_RECORD_LINE_MAX + 1U];
    char prefix[CONSOLE_RECORD_NAME_MAX + 8U];
    size_t effective = length;
    size_t prefix_length = 0U;
    size_t offset;

    if (record == NULL || length == 0U) {
        return;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (record[index] == '\n' || record[index] == '\r') {
            effective = index;
            break;
        }
    }
    if (effective <= CONSOLE_RECORD_MAX) {
        console_journal_record(record, effective);
        return;
    }
    console_journal_record(record, CONSOLE_RECORD_MAX);
    {
        const char *base = "RECORD";
        size_t base_length = 6U;
        size_t out = 0U;
        size_t open = 0U;
        size_t close;
        bool already_tail = false;

        while (open < effective && record[open] != '[') {
            ++open;
        }
        close = open + 1U;
        while (close < effective && record[close] != ']') {
            ++close;
        }
        if (open < effective && close < effective) {
            while (close > open + 1U && record[close - 1U] == ' ') {
                --close;
            }
            base = record + open + 1U;
            base_length = close - (open + 1U);
        }
        if (base_length >= 5U &&
            memcmp(base + base_length - 5U, "-TAIL", 5U) == 0) {
            already_tail = true;
        }
        prefix[out++] = '[';
        for (size_t index = 0U;
             index < base_length && out + 8U < sizeof(prefix); ++index) {
            prefix[out++] = base[index];
        }
        if (!already_tail) {
            for (size_t index = 0U; index < 5U; ++index) {
                prefix[out++] = "-TAIL"[index];
            }
        }
        prefix[out++] = ']';
        prefix[out] = '\0';
        prefix_length = out;
    }
    offset = CONSOLE_RECORD_MAX;
    while (offset < effective && prefix_length < CONSOLE_RECORD_MAX) {
        const size_t capacity = CONSOLE_RECORD_MAX - prefix_length;
        size_t chunk = effective - offset;

        if (chunk > capacity) {
            chunk = capacity;
        }
        for (size_t index = 0U; index < prefix_length; ++index) {
            buffer[index] = prefix[index];
        }
        for (size_t index = 0U; index < chunk; ++index) {
            buffer[prefix_length + index] = record[offset + index];
        }
        console_journal_record(buffer, prefix_length + chunk);
        ++console_record_split_parts;
        offset += chunk;
    }
}

static void console_service_line(const char *record, size_t length) {
    const uint64_t flags = serial_sequence_begin();

    for (size_t index = 0U; index < length; ++index) {
        serial_putc(record[index]);
    }
    serial_sequence_end(flags);
    ++console_service_lines;
    /* ITEM-481: a service line is bounded and split like every other record. */
    console_publish_journal(record, length);
}

void console_publish_display(uint64_t width, uint64_t height, uint64_t pitch,
                             uint32_t bpp, uint64_t mode_count,
                             const struct framebuffer_mode_choice *choice) {
    /*
     * ITEM-481: the buffer holds the worst case of the whole record (twenty
     * counters at twenty digits, the literals and the bounded `available` list),
     * not the journal bound, so no field is dropped here before the bounded
     * journal funnel can split the record into `[FB-TAIL]` parts.
     */
    char record[1024U];
    char available[160];
    uint64_t preferred_width = width;
    uint64_t preferred_height = height;
    uint64_t chosen_width = width;
    uint64_t chosen_height = height;
    uint32_t fallback = 0U;
    uint64_t available_count = 0U;
    size_t length = 0U;

    if (choice != NULL) {
        preferred_width = choice->preferred_width;
        preferred_height = choice->preferred_height;
        chosen_width = choice->chosen_width;
        chosen_height = choice->chosen_height;
        available_count = choice->available_count;
        fallback = choice->fallback;
    }
    available[0] = '\0';
    if (framebuffer_available_text(available, sizeof(available)) == 0U) {
        available[0] = '\0';
    }
    display_width = width;
    display_height = height;
    display_pitch = pitch;
    display_bpp = bpp;
    display_requested_width = preferred_width;
    display_requested_height = preferred_height;
    display_fallback = fallback;
    display_available_count = available_count;
    /* ITEM-466: fix the mode's glyph scale before the record quotes `cells=`. */
    console_scale = framebuffer_font_scale(width, height);
    console_cell_width = FRAMEBUFFER_FONT_CELL_WIDTH * console_scale;
    console_cell_height = FRAMEBUFFER_FONT_CELL_HEIGHT * console_scale;
    length = console_offer_record_append(record, sizeof(record), length,
                                         "[FB] width=");
    length = console_scroll_append_u64(record, sizeof(record), length, width);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " height=");
    length = console_scroll_append_u64(record, sizeof(record), length, height);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " pitch=");
    length = console_scroll_append_u64(record, sizeof(record), length, pitch);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " cells=");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       width / console_cell_width);
    length = console_offer_record_append(record, sizeof(record), length, "x");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       height / console_cell_height);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " bpp=");
    length = console_scroll_append_u64(record, sizeof(record), length, bpp);
    /*
     * ITEM-464/466: how many modes the firmware offered and the mode the
     * selection rule picked from them.  ITEM-466 makes `requested=` the
     * firmware's own preferred mode: the boot no longer asks for one number of
     * its own, so the field is the legacy name of `preferred=` beside it.
     */
    length = console_offer_record_append(record, sizeof(record), length,
                                         " modes=");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       mode_count);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " chosen=");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       chosen_width);
    length = console_offer_record_append(record, sizeof(record), length, "x");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       chosen_height);
    /*
     * ITEM-463/466: the preferred mode and the one the firmware actually
     * gave.  `fallback=1` is "the native mode was not available and a larger
     * one was taken instead": the console can then be slow for a reason the
     * record names, and a hardware reader does not have to guess whether the
     * 4K screen was requested or forced.
     */
    length = console_offer_record_append(record, sizeof(record), length,
                                         " requested=");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       preferred_width);
    length = console_offer_record_append(record, sizeof(record), length, "x");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       preferred_height);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " actual=");
    length = console_scroll_append_u64(record, sizeof(record), length, width);
    length = console_offer_record_append(record, sizeof(record), length, "x");
    length = console_scroll_append_u64(record, sizeof(record), length, height);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " fallback=");
    length = console_scroll_append_u64(record, sizeof(record), length, fallback);
    /*
     * ITEM-466: the mode decides the glyph scale, and the scale decides the
     * cell.  The console is not up yet when this record is built, so the pure
     * rule is applied here; console_init() repeats it for the paint path.  The
     * two fields are appended after `fallback=`, so every record shape pinned
     * by an earlier item keeps its prefix.
     */
    length = console_offer_record_append(record, sizeof(record), length,
                                         " scale=");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       console_scale);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " cell_px=");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       console_cell_width);
    length = console_offer_record_append(record, sizeof(record), length, "x");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       console_cell_height);
    /*
     * ITEM-466: the firmware's own preference and the list it offered.  The
     * owner's rule is "the native mode, with a fallback to the largest
     * available", and these two fields are what let a reader check it: the
     * record names the preferred mode, the count of 32-bpp modes offered and
     * their list (bounded; `...` marks a list that did not fit).
     */
    length = console_offer_record_append(record, sizeof(record), length,
                                         " preferred=");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       preferred_width);
    length = console_offer_record_append(record, sizeof(record), length, "x");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       preferred_height);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " available_count=");
    length = console_scroll_append_u64(record, sizeof(record), length,
                                       available_count);
    length = console_offer_record_append(record, sizeof(record), length,
                                         " available=");
    length = console_offer_record_append(record, sizeof(record), length,
                                         available);
    /*
     * ITEM-461: serial as well as the journal.  The serial capture is what a
     * proof holds; the journal is what the next boot reads.  ITEM-478: the
     * display geometry is a service record, so it reaches those two and not
     * the screen the operator sees; the `kbd` command prints the same numbers
     * on the screen when they are asked for.
     */
    {
        const uint64_t flags = serial_sequence_begin();

        for (size_t index = 0U; index < length; ++index) {
            serial_putc(record[index]);
        }
        serial_putc('\n');
        serial_sequence_end(flags);
    }
    console_publish_journal(record, length);
}

/* ITEM-461: the line-paint counters the heartbeat publishes on the serial
   stream, where the QEMU proof can read them, and which the journal keeps as
   their own `[CONSOLE] paint` record. */
uint64_t console_render_line_count(void) {
    return console_paint_lines;
}

uint64_t console_render_char_count(void) {
    return console_paint_chars;
}

uint64_t console_render_max_us(void) {
    return console_paint_max_us;
}

uint64_t console_render_paint_us(void) {
    return console_paint_average_us();
}

uint64_t console_render_scroll_us(void) {
    return console_scroll_average_us();
}

uint64_t console_render_scroll_max_us(void) {
    return console_scroll_max_us;
}

uint64_t console_render_pointer_moves(void) {
    return console_pointer_moves;
}

/* ITEM-466: the records the screen budget kept off the screen this boot. */
uint64_t console_render_screen_suppressed(void) {
    return console_screen_suppressed;
}

#ifdef HFOC_CONSOLE_HOST_TEST
/* ITEM-466: the host proof starts each case with a fresh one-second window. */
void console_reset_screen_budget(void) {
    console_screen_window_ticks = 0U;
    console_screen_window_painted = 0U;
    console_screen_suppressed = 0U;
}

/* ITEM-474: the rows the batched scroll has moved, for the batch host proof. */
uint64_t console_test_scroll_rows(void) {
    return console_scroll_rows;
}

/*
 * ITEM-509 increment 24: the two refusals whose branch the host build cannot
 * reach.  `console_power_refusal()` sits behind the platform-power build guard
 * and `console_admin_credentials_begin()` is reached only through the keyboard
 * path, which a live conversation captures; both hooks call the same functions
 * the kernel calls, so the proof reads the exact line rather than a copy.
 */
void console_test_power_refusal(const char *action) {
    console_power_refusal(action);
}

void console_test_admin_credentials_reenter(void) {
    console_admin_credentials_begin(
        APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_CHANGE);
}
#endif

uint64_t console_render_pointer_max_us(void) {
    return console_pointer_max_us;
}

static bool console_record_seen(const char *line)
{
    size_t index;

    for (index = 0U; index < record_dedup_count; ++index) {
        if (strcmp(record_dedup[index], line) == 0) {
            return true;
        }
    }
    return false;
}

static void console_record_remember(const char *line)
{
    size_t index = 0U;

    while (line[index] != '\0' && index < CONSOLE_RECORD_LINE_MAX) {
        record_dedup[record_dedup_next][index] = line[index];
        ++index;
    }
    record_dedup[record_dedup_next][index] = '\0';
    record_dedup_next = (record_dedup_next + 1U) % CONSOLE_RECORD_DEDUP;
    if (record_dedup_count < CONSOLE_RECORD_DEDUP) {
        ++record_dedup_count;
    }
}

static void console_record_paint(const char *line)
{
    size_t index = 0U;
    /* ITEM-461: a mirrored record is one contiguous paint burst. */
    const uint64_t paint_started = timer_cycles();
    const uint64_t paint_scroll_before = console_scroll_cycles_total;

    /* ITEM-466: the screen budget.  The record is already in the journal; a
       suppressed paint is counted, not lost, and the count is published. */
    if (!console_screen_record_allow()) {
        return;
    }

    /* ITEM-448: the same rule as console_write(): the arrow is not screen
       content, so it is put away while a record is painted and shown again
       afterwards, on every return path. */
    pointer_suspend();

    /*
     * A record is never painted into the middle of a command the operator is
     * typing: the typed line is ended first, and the prompt and what has been
     * typed so far are re-emitted after the record so the operator can carry on
     * where they were.  Both are framebuffer-only, like the record itself.
     */
    if (input_length != 0U) {
        console_emit('\n', false);
    }
    while (line[index] != '\0') {
        console_emit(line[index], false);
        ++index;
    }
    console_emit('\n', false);
    if (input_length != 0U) {
        static const char prompt_text[] = "hfoc> ";

        for (index = 0U; prompt_text[index] != '\0'; ++index) {
            console_emit(prompt_text[index], false);
        }
        for (index = 0U; index < input_length; ++index) {
            console_emit(input_line[index], false);
        }
        for (index = input_length; index > input_cursor; --index) {
            console_emit('\b', false);
        }
    }
    pointer_resume();
    /* ITEM-464: drain the record's write-combining stores before the cost. */
    console_sync();
    console_paint_record_lines(
        1U, timer_cycles() - paint_started -
                (console_scroll_cycles_total - paint_scroll_before));
}

/*
 * A record published before the banner is buffered, not painted: console_init()
 * clears the framebuffer, so a record painted earlier would be erased before
 * anyone could read it.  The FIFO drops its oldest entry when it is full - the
 * newest record is the one that explains how the boot ended.
 */
static void console_record_hold(const char *line)
{
    size_t index = 0U;
    size_t slot;

    if (record_pending_count == CONSOLE_RECORD_PENDING) {
        record_pending_head =
            (record_pending_head + 1U) % CONSOLE_RECORD_PENDING;
        --record_pending_count;
    }
    slot = (record_pending_head + record_pending_count) %
           CONSOLE_RECORD_PENDING;
    while (line[index] != '\0' && index < CONSOLE_RECORD_LINE_MAX) {
        record_pending[slot][index] = line[index];
        ++index;
    }
    record_pending[slot][index] = '\0';
    ++record_pending_count;
}

static void console_record_flush(void)
{
    while (record_pending_count > 0U) {
        const char *line = record_pending[record_pending_head];

        record_pending_head =
            (record_pending_head + 1U) % CONSOLE_RECORD_PENDING;
        --record_pending_count;
        console_record_paint(line);
    }
}

void console_publish_record(const char *record, size_t length)
{
    char line[CONSOLE_RECORD_LINE_MAX + 1U];

    if (!console_record_accept(record, length, line)) {
        return;
    }
    /*
     * ITEM-410: the same bounded line the console is about to paint is handed
     * to the boot log, before the screen's own de-duplication: a record the
     * console chooses not to repaint is still a record the appliance published,
     * and the log keeps its own, narrower rule (see
     * `appliance_boot_log_store_observe()`).
     */
    if (boot_log_bound) {
        boot_log_binding.observe(line, strlen(line));
    }
    /* Identical consecutive records paint once: the screen must not scroll. */
    if (console_record_seen(line)) {
        return;
    }
    console_record_remember(line);
    if (!console_ready) {
        console_record_hold(line);
        return;
    }
    console_record_paint(line);
}

/*
 * ITEM-405: the fragment-by-fragment form of the mirror.
 *
 * `console_record_text()`/`_u64()`/`_hex64()` only append, so they are cheap
 * enough to call inside a serial sequence where interrupts are disabled, and
 * the single paint happens in `console_record_end()` after that sequence has
 * ended.  The decimal and hexadecimal renderings match the serial helpers the
 * same fragments were written with, so the console line is the serial line.
 */
static char mirror_text[CONSOLE_RECORD_ASSEMBLY_MAX + 1U];
static size_t mirror_length;
static bool mirror_active;
static bool mirror_complete;
static bool mirror_overflow;
/*
 * ITEM-477: which funnel the assembled line is handed to when it ends.  The
 * fragments are shared with the journal-only form, so the flag - not a second
 * buffer - is what tells `console_mirror_finish()` whether the record paints.
 */
static bool mirror_to_journal;
/*
 * ITEM-481: the running worst case, its peak, the record's own name and the
 * offsets at which the published record must be broken so that every part's
 * worst case fits CONSOLE_RECORD_MAX.  The breaks are decided while the record
 * is assembled but nothing is published until `console_mirror_finish()`, after
 * the serial sequence has ended, so the log's own `[BOOTLOG]` serial line
 * cannot split the line the sequence is still writing.
 */
static size_t mirror_worst;
static size_t mirror_worst_peak;
static size_t mirror_bound_violations;
static char mirror_name[CONSOLE_RECORD_NAME_MAX + 1U];
static size_t mirror_splits[CONSOLE_RECORD_MAX_PARTS];
static size_t mirror_split_count;

static void console_mirror_open(bool to_journal)
{
    /*
     * ITEM-483: assembling a record is foreign output even when it never paints
     * the framebuffer.  A journal-only record is written to the serial stream
     * around the typed characters, so the next keystroke must re-emit the
     * prompt and the whole line there; opening the record is the one point
     * every record passes through before its first fragment.  A painted record
     * would bump the counter through its own `console_emit()` calls anyway, so
     * this only makes the rule uniform.
     */
    ++console_output_generation;
    mirror_length = 0U;
    mirror_active = true;
    mirror_complete = false;
    mirror_overflow = false;
    mirror_to_journal = to_journal;
    mirror_worst = 0U;
    mirror_worst_peak = 0U;
    mirror_name[0] = '\0';
    mirror_split_count = 0U;
    mirror_text[0] = '\0';
}

void console_record_begin(void)
{
    console_mirror_open(false);
}

void console_journal_begin(void)
{
    console_mirror_open(true);
}

size_t console_record_worst_length(void)
{
    return mirror_worst_peak;
}

size_t console_record_spill_count(void)
{
    return console_record_split_parts;
}

size_t console_record_bound_violations(void)
{
    return mirror_bound_violations;
}

static void console_mirror_put(char value)
{
    if (!mirror_active || mirror_complete) {
        return;
    }
    if (mirror_length >= CONSOLE_RECORD_ASSEMBLY_MAX) {
        mirror_overflow = true;
        return;
    }
    mirror_text[mirror_length++] = value;
    mirror_text[mirror_length] = '\0';
}

static void console_mirror_count(size_t worst)
{
    mirror_worst += worst;
    if (mirror_worst > mirror_worst_peak) {
        mirror_worst_peak = mirror_worst;
    }
}

/*
 * ITEM-481: remember the record's own name - the first `[NAME]` in the first
 * fragment - so a continuation can be published as `[NAME-TAIL]`.
 */
static void console_mirror_capture_name(const char *text)
{
    size_t start = 0U;
    size_t end;

    if (text == NULL || mirror_name[0] != '\0') {
        return;
    }
    while (text[start] != '\0' && text[start] != '[') {
        ++start;
    }
    if (text[start] != '[') {
        return;
    }
    end = start + 1U;
    while (text[end] != '\0' && text[end] != ']' &&
           end - start <= CONSOLE_RECORD_NAME_MAX) {
        ++end;
    }
    if (text[end] != ']') {
        return;
    }
    while (end > start + 1U && text[end - 1U] == ' ') {
        --end;
    }
    {
        const size_t length = end - start + 1U;

        for (size_t index = 0U; index < length; ++index) {
            mirror_name[index] = text[start + index];
        }
        mirror_name[length] = '\0';
    }
}

/*
 * ITEM-481: record a break point at the current position and reset the running
 * worst case to the worst case of a fresh `-TAIL` prefix.  Nothing is published
 * here; `console_mirror_finish()` turns the offsets into the parts after the
 * serial sequence has ended.
 */
static void console_mirror_break(void)
{
    if (!mirror_to_journal) {
        return;
    }
    if (mirror_split_count >= CONSOLE_RECORD_MAX_PARTS) {
        mirror_overflow = true;
        return;
    }
    if (mirror_split_count == 0U && mirror_length == 0U) {
        return;
    }
    if (mirror_split_count != 0U &&
        mirror_splits[mirror_split_count - 1U] == mirror_length) {
        return;
    }
    mirror_splits[mirror_split_count++] = mirror_length;
    mirror_worst = CONSOLE_RECORD_TAIL_WORST;
}

/* Break before a value whose worst case no longer fits the current part. */
static void console_mirror_reserve(size_t worst)
{
    if (mirror_to_journal && mirror_worst + worst > CONSOLE_RECORD_MAX) {
        console_mirror_break();
    }
}

#ifndef HFOC_CONSOLE_HOST_TEST
/*
 * ITEM-557: the refusal as one durable record.  The console's journal funnel
 * is the boot log, which lives on a different placement from the diagnostic
 * journal; a save that could not reach its own medium therefore still leaves
 * `[DIAG-FAIL]` behind for `bootlog` and the Debian reader of the boot log.
 */
static void diag_failure_record(const struct diag_failure *failure) {
    struct record_line record;

    if (failure == NULL || !failure->recorded) {
        return;
    }
    record_line_begin(&record);
    record_line_append(&record, "[DIAG-FAIL] reason=");
    record_line_append(&record, failure->reason);
    record_line_append(&record, " step=");
    record_line_append(&record, failure->step);
    if (failure->primary[0] != '\0') {
        record_line_append(&record, " primary=");
        record_line_append(&record, failure->primary);
    }
    if (!record.overflow) {
        console_journal_record(record.text, record.length);
    }
}
#endif

void console_record_text(const char *text)
{
    size_t length = 0U;
    size_t index = 0U;
    size_t lookahead = 0U;
    bool broken = false;

    if (text == NULL) {
        return;
    }
    console_mirror_capture_name(text);
    while (text[length] != '\0' && text[length] != '\n' &&
           text[length] != '\r') {
        ++length;
    }
    /*
     * ITEM-561: the terminator is recorded *after* the fragment is carried.
     * Setting the flag here made every `console_mirror_put` a no-op for a
     * fragment that ends its record, so a fragment longer than one part could
     * not shorten the running worst case: the break was recorded once, the
     * second `continue` found the same offset, and the loop never advanced.
     * A ~430-character start record hung the machine on that path.
     */
    /*
     * A fragment that ends a field name reserves the field's value too, so a
     * break falls between fields and never between `name=` and its number.
     */
    if (length != 0U && text[length - 1U] == '=') {
        lookahead = CONSOLE_RECORD_U64_WORST;
    }
    while (index < length) {
        const size_t remaining = length - index;
        const size_t wanted = remaining + (broken ? 0U : lookahead);
        size_t room;

        if (!mirror_to_journal || mirror_worst + wanted <= CONSOLE_RECORD_MAX) {
            for (size_t step = 0U; step < remaining; ++step) {
                console_mirror_put(text[index + step]);
            }
            console_mirror_count(remaining);
            break;
        }
        /*
         * The fragment does not fit where the part stands.  If it fits in a
         * fresh part at all, break once and keep it whole; only a fragment
         * longer than a part is cut, and even then every byte is carried into
         * the next continuation.
         */
        if (wanted <= CONSOLE_RECORD_MAX && !broken && mirror_worst != 0U) {
            console_mirror_break();
            broken = true;
            continue;
        }
        room = mirror_worst < CONSOLE_RECORD_MAX
                   ? CONSOLE_RECORD_MAX - mirror_worst
                   : 0U;
        if (room == 0U) {
            console_mirror_break();
            broken = true;
            continue;
        }
        for (size_t step = 0U; step < room; ++step) {
            console_mirror_put(text[index + step]);
        }
        console_mirror_count(room);
        index += room;
        broken = false;
    }
    if (text[length] == '\n' || text[length] == '\r') {
        mirror_complete = true;
    }
}

void console_record_u64(uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    console_mirror_reserve(CONSOLE_RECORD_U64_WORST);
    if (value == 0U) {
        console_mirror_put('0');
        console_mirror_count(CONSOLE_RECORD_U64_WORST);
        return;
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        console_mirror_put(digits[--count]);
    }
    console_mirror_count(CONSOLE_RECORD_U64_WORST);
}

void console_record_hex64(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    /*
     * Exactly what `serial_write_hex()` prints, so a record assembled from
     * both helpers mirrors the serial line character for character: a "0x"
     * prefix and sixteen uppercase hex digits.
     */
    console_mirror_reserve(CONSOLE_RECORD_HEX64_WORST);
    console_mirror_put('0');
    console_mirror_put('x');
    for (shift = 60; shift >= 0; shift -= 4) {
        console_mirror_put(digits[(value >> (unsigned int)shift) & 0x0FU]);
    }
    console_mirror_count(CONSOLE_RECORD_HEX64_WORST);
}

/*
 * ITEM-481: the prefix a continuation carries.  A record already named
 * `[X-TAIL]` keeps that name, so a record split twice publishes `[X-TAIL]`
 * again rather than `[X-TAIL-TAIL]`; every other record becomes `[X-TAIL]`.
 */
static size_t console_mirror_tail_prefix(char *output, size_t capacity)
{
    const char *base = mirror_name;
    size_t base_length = 0U;
    size_t length = 0U;
    bool already_tail = false;

    if (base[0] == '[') {
        ++base;
    }
    while (base[base_length] != '\0' && base[base_length] != ']') {
        ++base_length;
    }
    if (base_length == 0U) {
        base = "RECORD";
        base_length = 6U;
    }
    if (base_length >= 5U &&
        memcmp(base + base_length - 5U, "-TAIL", 5U) == 0) {
        already_tail = true;
    }
    output[length++] = '[';
    for (size_t index = 0U; index < base_length && length + 8U < capacity;
         ++index) {
        output[length++] = base[index];
    }
    if (!already_tail) {
        for (size_t index = 0U; index < 5U && length + 2U < capacity;
             ++index) {
            output[length++] = "-TAIL"[index];
        }
    }
    output[length++] = ']';
    output[length] = '\0';
    return length;
}

/*
 * ITEM-481: turn the break offsets recorded during assembly into the published
 * parts.  The head keeps the record's own name; every continuation carries
 * `[<name>-TAIL]`.  Each part's worst case was bounded by CONSOLE_RECORD_MAX
 * while it was assembled, so every published line is inside the bound without
 * cutting a byte.
 */
static void console_mirror_publish_journal(void)
{
    char prefix[CONSOLE_RECORD_NAME_MAX + 8U];
    char part[CONSOLE_RECORD_LINE_MAX + 1U];
    size_t prefix_length = 0U;
    size_t start = 0U;

    for (size_t index = 0U; index <= mirror_split_count; ++index) {
        const size_t end = index < mirror_split_count
                               ? mirror_splits[index]
                               : mirror_length;

        if (index == 0U) {
            if (end > start) {
                console_journal_record(mirror_text + start, end - start);
            }
        } else {
            size_t chunk = end - start;

            if (prefix_length == 0U) {
                prefix_length = console_mirror_tail_prefix(
                    prefix, sizeof(prefix));
            }
            if (chunk > CONSOLE_RECORD_MAX - prefix_length) {
                chunk = CONSOLE_RECORD_MAX - prefix_length;
            }
            for (size_t step = 0U; step < prefix_length; ++step) {
                part[step] = prefix[step];
            }
            for (size_t step = 0U; step < chunk; ++step) {
                part[prefix_length + step] = mirror_text[start + step];
            }
            if (prefix_length + chunk != 0U) {
                console_journal_record(part, prefix_length + chunk);
                ++console_record_split_parts;
            }
        }
        start = end;
    }
}

/*
 * ITEM-477: the opener chooses the funnel and the shared finish below sends the
 * assembled line to it, so the fragment helpers above stay the one
 * implementation whether the record paints or only reaches the journal.
 */
static void console_mirror_finish(void)
{
    const bool to_journal = mirror_to_journal;

    if (!mirror_active) {
        return;
    }
    mirror_active = false;
    if (mirror_length == 0U) {
        return;
    }
    if (to_journal) {
        /*
         * ITEM-481: the assembly buffer is the worst case of the longest
         * builder, so this should be impossible.  If a future builder ever
         * outgrows it anyway, the fact is published as its own bounded record
         * instead of being lost with a silent "..."; the QEMU proof reads the
         * journal and fails on any `[CONSOLE-BOUND]`.
         */
        if (mirror_overflow) {
            char bound[64];
            size_t bound_length = 0U;

            ++mirror_bound_violations;
            bound_length = console_offer_record_append(
                bound, sizeof(bound), bound_length, "[CONSOLE-BOUND] worst=");
            bound_length = console_scroll_append_u64(
                bound, sizeof(bound), bound_length,
                (uint64_t)mirror_length);
            bound_length = console_offer_record_append(
                bound, sizeof(bound), bound_length, " max=");
            bound_length = console_scroll_append_u64(
                bound, sizeof(bound), bound_length,
                (uint64_t)CONSOLE_RECORD_ASSEMBLY_MAX);
            console_journal_record(bound, bound_length);
        }
        console_mirror_publish_journal();
    } else {
        console_publish_record(mirror_text, mirror_length);
    }
    mirror_length = 0U;
    mirror_text[0] = '\0';
}

void console_record_end(void)
{
    console_mirror_finish();
}

void console_journal_end(void)
{
    console_mirror_finish();
}

void console_publish_boot_persistence(
    bool storage_ready,
    uint32_t backend_count,
    enum appliance_config_validation_source config_source,
    const char *hostname,
    size_t hostname_length)
{
    if (hostname == NULL ||
        hostname_length == 0U ||
        hostname_length > APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX ||
        (config_source != APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
         config_source !=
             APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING &&
         config_source !=
             APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID)) {
        return;
    }
    for (size_t index = 0U; index < hostname_length; ++index) {
        boot_hostname[index] = hostname[index];
    }
    boot_hostname[hostname_length] = '\0';
    boot_storage_ready = storage_ready;
    boot_backend_count = backend_count;
    boot_config_source = config_source;
    boot_persistence_published = true;
}

void console_process_input(char value) {
    if (value == '\n') {
        console_putc('\n');
        execute_command();
        reset_input_state();
        return;
    }
    if (value == '\b') {
        if (input_length != 0U) {
            --input_length;
            if (input_cursor > input_length) {
                input_cursor = input_length;
            }
            console_putc('\b');
        }
        return;
    }
    if (value == '\t') {
        return;
    }
    if (value >= 32 && value <= 126 && input_length < INPUT_MAX) {
        input_line[input_length++] = value;
        input_cursor = input_length;
        console_putc(value);
    }
}

/*
 * One implementation of every action: the menu runs the console's own command
 * words instead of a second table of helpers, so `appliance`, `kbd` and `pmm`
 * behave the same whether they were typed or chosen.
 */
static void console_run_command(const char *word) {
    size_t index = 0U;

    if (word == NULL) {
        return;
    }
    while (word[index] != '\0' && index < INPUT_MAX) {
        input_line[index] = word[index];
        ++index;
    }
    input_line[index] = '\0';
    input_length = index;
    input_cursor = index;
    execute_command();
}

/*
 * ITEM-482: the wait between the USB completion decoding a report and the
 * console taking charge of its character.  The fast path hands the cell to the
 * painter here, so the number measures the echo path's own work; the turn's
 * flush is bounded by the loop budget and is where the paint lands.  Only a USB
 * event carries a completion clock; a PS/2 event reports zero and is not
 * measured rather than counted as instant.
 */
static void console_note_keyboard_echo(const struct keyboard_event *event) {
    const uint64_t now = timer_cycles();
    uint64_t micros;

    if (event == NULL || event->reported_cycles == 0U ||
        now < event->reported_cycles) {
        return;
    }
    micros = timer_cycles_to_us(now - event->reported_cycles);
    if (micros > console_kbd_echo_us_max) {
        console_kbd_echo_us_max = micros;
    }
}

void console_process_event(const struct keyboard_event *event) {
    if (event == 0) {
        return;
    }
    /*
     * ITEM-430: any keystroke is the operator being there, so the machine
     * never offers a menu to someone who is already typing.  The one keystroke
     * that does not cancel an offer is the empty-line ENTER that opens it.
     */
    console_input_seen = true;
    /* A menu that is open owns the keyboard: a keystroke aimed at it must not
       reach the line editor underneath it. */
    if (local_menu_active(&menu_state)) {
        local_menu_handle_event(event);
        return;
    }
    if (pager_active) {
        pager_handle_event(event);
        return;
    }
    if (menu_offer_open &&
        !(event->type == KEYBOARD_EVENT_ENTER && input_length == 0U)) {
        menu_offer_open = false;
        console_service_line("[LOCAL-MENU] offer cancelled\n",
                             strlen("[LOCAL-MENU] offer cancelled\n"));
    }

    /*
     * ITEM-585: the operator-entropy seed is a hidden line too.  It is not a
     * password, but it is the root of this boot's TLS randomness, so it must
     * not reach the screen, the serial transcript, the history or the journal.
     * The line is bounded by the same INPUT_MAX and refused as too long rather
     * than cut.
     */
    if (entropy_seed_active) {
        switch (event->type) {
        case KEYBOARD_EVENT_CHARACTER:
            if (event->character >= 32 && event->character <= 126) {
                if (input_length < INPUT_MAX) {
                    input_line[input_length++] = event->character;
                    input_cursor = input_length;
                    input_line[input_length] = '\0';
                } else {
                    input_overflow = true;
                }
            }
            break;
        case KEYBOARD_EVENT_BACKSPACE:
            if (input_length != 0U) {
                --input_length;
                input_cursor = input_length;
                input_line[input_length] = '\0';
                input_overflow = false;
            }
            break;
        case KEYBOARD_EVENT_ENTER:
            console_putc('\n');
            console_entropy_seed_enter();
            break;
        default:
            break;
        }
        console_keyboard_echo_active = false;
        return;
    }

    /*
     * ITEM-506/ITEM-610: while the account conversation is open the keyboard
     * feeds its own line instead of the line editor.  The login is painted as
     * it is typed and a password step paints one `*` per accepted character;
     * the echo is framebuffer-only, nothing is added to the history or the
     * saved draft, and ENTER hands the line to the conversation rather than the
     * command chain, so a typed password never reaches the serial transcript
     * or the durable journal.  An empty line is the way out, and a line that
     * overflowed the buffer is remembered so it is refused as too long instead
     * of being cut.
     */
    if (admin_credentials_active) {
        /* The feedback is the operator's echo: the turn's one drain must carry
           it, and it must not invalidate the normal line editor's fast path. */
        console_keyboard_echo_active = true;
        switch (event->type) {
        case KEYBOARD_EVENT_CHARACTER:
            if (event->character >= 32 && event->character <= 126) {
                if (input_length < INPUT_MAX) {
                    input_line[input_length++] = event->character;
                    input_cursor = input_length;
                    input_line[input_length] = '\0';
                    console_admin_credentials_echo_character(
                        event->character);
                } else {
                    input_overflow = true;
                }
            }
            break;
        case KEYBOARD_EVENT_BACKSPACE:
            if (input_length != 0U) {
                console_admin_credentials_echo_backspace();
                --input_length;
                input_cursor = input_length;
                input_line[input_length] = '\0';
                /*
                 * The line can no longer exceed the cap once a character is
                 * gone, so a refusal must not stick to a line the operator has
                 * since shortened to a legal length.
                 */
                input_overflow = false;
            }
            break;
        case KEYBOARD_EVENT_ENTER:
            console_putc('\n');
            console_admin_credentials_enter();
            break;
        default:
            break;
        }
        console_keyboard_echo_active = false;
        return;
    }

    /*
     * ITEM-482 (round 20): everything below is the keyboard echo, so the
     * write-combining drains it causes are charged to `kbd_sync_us`.
     */
    console_keyboard_echo_active = true;
    switch (event->type) {
    case KEYBOARD_EVENT_CHARACTER:
        if (event->character >= 32 && event->character <= 126 &&
            input_length < INPUT_MAX) {
            if (input_cursor == input_length) {
                /*
                 * ITEM-482: a character appended at the end of the line is one
                 * painted cell.  The old path shifted the line, called
                 * `redraw_input_line()` - which writes `"\r"`, the prompt and
                 * the whole line, each with its own drain - and made the echo
                 * cost grow with the line's length.  The prompt and the text
                 * already on the screen stay put, so only the new cell is
                 * painted.  The full redraw stays for edits that move the text
                 * (mid-line insert, LEFT/RIGHT/HOME/END/DELETE, history).
                 *
                 * ITEM-483: "stay put" is only true while no other writer has
                 * touched the screen since the line was rendered.  When the
                 * generation moved, the one-cell paint would land on a stale
                 * cursor and the serial witness would lose the prompt, so the
                 * whole line is redrawn instead.
                 */
                input_line[input_cursor] = event->character;
                ++input_cursor;
                ++input_length;
                input_line[input_length] = '\0';
                if (console_input_line_generation ==
                        console_output_generation &&
                    previous_rendered_length + 1U == input_length) {
                    previous_rendered_length = input_length;
                    /* MOUSE-DIAG (2026-09-27): the fast path's own count, so a
                       fast path that stops being taken is visible. */
                    ++console_kbd_echo_fast_count;
                    console_putc(event->character);
                } else {
                    /* The generation moved: this is the ITEM-483 invalidation
                       the contract asks to count separately from the text
                       edits that redraw by definition. */
                    if (console_input_line_generation !=
                        console_output_generation) {
                        ++console_kbd_echo_generation_invalidations;
                    }
                    redraw_input_line();
                }
            } else {
                for (size_t index = input_length; index > input_cursor; --index) {
                    input_line[index] = input_line[index - 1U];
                }
                input_line[input_cursor] = event->character;
                ++input_length;
                ++input_cursor;
                input_line[input_length] = '\0';
                redraw_input_line();
            }
            console_note_keyboard_echo(event);
        }
        break;
    case KEYBOARD_EVENT_ENTER:
        console_putc('\n');
        console_note_keyboard_echo(event);
        /*
         * ITEM-482: the command's own output is not the keystroke's drain, so
         * stop charging sync time before it runs.
         */
        console_keyboard_echo_active = false;
        if (local_network_setup_active(&network_setup)) {
            console_network_setup_enter();
            break;
        }
        execute_command();
        break;
    case KEYBOARD_EVENT_BACKSPACE:
        if (input_cursor != 0U) {
            if (input_cursor == input_length) {
                /*
                 * ITEM-482: erasing the last character is one painted cell too.
                 * `console_putc('\b')` erases the cell at the moved cursor; the
                 * full redraw is kept for a backspace inside the line, where the
                 * tail has to shift.
                 *
                 * ITEM-483: after foreign output the cursor is not on the
                 * cell the backspace would erase, so the full redraw re-emits
                 * the prompt and the shortened line instead.
                 */
                --input_length;
                --input_cursor;
                input_line[input_length] = '\0';
                if (console_input_line_generation ==
                        console_output_generation &&
                    previous_rendered_length == input_length + 1U) {
                    previous_rendered_length = input_length;
                    console_putc('\b');
                } else {
                    redraw_input_line();
                }
            } else {
                for (size_t index = input_cursor; index < input_length; ++index) {
                    input_line[index - 1U] = input_line[index];
                }
                --input_length;
                --input_cursor;
                input_line[input_length] = '\0';
                redraw_input_line();
            }
        }
        break;
    case KEYBOARD_EVENT_DELETE:
        if (input_cursor < input_length) {
            for (size_t index = input_cursor; index + 1U < input_length;
                 ++index) {
                input_line[index] = input_line[index + 1U];
            }
            --input_length;
            input_line[input_length] = '\0';
            redraw_input_line();
        }
        break;
    case KEYBOARD_EVENT_LEFT:
        if (input_cursor != 0U) {
            --input_cursor;
            redraw_input_line();
        }
        break;
    case KEYBOARD_EVENT_RIGHT:
        if (input_cursor < input_length) {
            ++input_cursor;
            redraw_input_line();
        }
        break;
    case KEYBOARD_EVENT_HOME:
        input_cursor = 0U;
        redraw_input_line();
        break;
    case KEYBOARD_EVENT_END:
        input_cursor = input_length;
        redraw_input_line();
        break;
    case KEYBOARD_EVENT_UP:
        if (history_count == 0U) {
            break;
        }
        if (!history_navigation_active) {
            save_draft();
            history_navigation_active = true;
            history_navigation_offset = 0U;
            load_history_entry(0U);
        } else if (history_navigation_offset + 1U < history_count) {
            ++history_navigation_offset;
            load_history_entry(history_navigation_offset);
        } else {
            redraw_input_line();
        }
        break;
    case KEYBOARD_EVENT_DOWN:
        if (!history_navigation_active) {
            break;
        }
        if (history_navigation_offset == 0U) {
            restore_draft();
            history_navigation_active = false;
            history_navigation_offset = 0U;
        } else {
            --history_navigation_offset;
            load_history_entry(history_navigation_offset);
        }
        break;
    }
    console_keyboard_echo_active = false;
}

void console_update_uptime(uint64_t seconds) {
    /* ITEM-648: the right-hand uptime block is gone from the operator screen.
       The clock and the `uptime` command keep their values; the second event
       still reaches this entry point, which now has nothing to repaint. */
    (void)seconds;
}

#ifdef HFOC_CONSOLE_HOST_TEST
void console_test_reset(void) {
    reset_history_state();
    reset_input_state();
    input_line[0] = '\0';
    cursor_x = LEFT;
    cursor_y = BODY_TOP + 8U;
    /* ITEM-483: the output generation is boot state too, so a case starts with
       the fast path's baseline equal to an untouched stream. */
    console_output_generation = 0U;
    console_input_line_generation = 0U;
    /*
     * ITEM-405: the record mirror is boot state too, so a host case starts from
     * the same place the boot does - no dedup history, no buffered records and
     * the pre-banner state, which is what a test drives console_init() out of.
     */
    record_dedup_next = 0U;
    record_dedup_count = 0U;
    record_pending_head = 0U;
    record_pending_count = 0U;
    console_ready = false;
    management_published = false;
    management_url[0] = '\0';
    management_pending = false;
    management_pending_url[0] = '\0';
    management_pending_reachability[0] = '\0';
    management_pending_certificate[0] = '\0';
    mirror_length = 0U;
    mirror_active = false;
    mirror_complete = false;
    mirror_text[0] = '\0';
    /*
     * ITEM-430: the menu and the offer it makes are boot state too, so a host
     * case starts where the boot does - no menu open, no offer made, and no
     * keystroke counted against the idle window.
     */
    local_menu_close(&menu_state);
    pointer_target_count = 0U;
    pointer_published_targets = 0U;
    pointer_ready = false;
    pointer_initial_shown = false;
    pointer_now = (struct pointer_state){0};
    /* ITEM-484: the input batch's drain flag and echo marker are boot state
       too, so a case never inherits a pending drain from an earlier one. */
    console_input_needs_sync = false;
    console_keyboard_echo_active = false;
    /*
     * ITEM-472: a network wizard an earlier case opened must not swallow this
     * case's command line, so the setup is reset with the rest of the boot
     * state.
     */
    local_network_setup_abort(&network_setup);
    menu_offer.started = false;
    menu_offer.first_ms = 0U;
    menu_offer.offered = false;
    menu_offer_open = false;
    console_input_seen = false;
    /*
     * ITEM-457: the scroll counters are boot state too, so a host case that
     * drives the console to the bottom of the screen reads the rows and bytes
     * it caused rather than the ones an earlier case did.
     */
    console_scroll_rows = 0U;
    console_scroll_bytes = 0U;
    console_scroll_window_rows = 0U;
    console_scroll_window_us = 0U;
    console_scroll_max_us = 0U;
    console_scroll_reported_rows = 0U;
    console_scroll_reported_max_us = 0U;
    console_scroll_publishing = false;
    /* ITEM-474: no case may inherit an open scroll batch or a batching flag. */
    console_scroll_pending_cells = 0U;
    console_defer_length = 0U;
    console_deferring = false;
    console_replaying = false;
    console_defer_batching = false;
    console_flush_active = false;
    console_write_depth = 0U;
    console_cost_us_total = 0U;
    /* ITEM-509 increment 4: the language, its binding and the record behind
       them are boot state too, so a case never inherits a switch. */
    language_binding = NULL;
    console_language = APPLIANCE_LANGUAGE_DEFAULT;
    console_language_durable = false;
    /* ITEM-516: the gate's binding, setting and local proof are boot state. */
    gate_binding = NULL;
    console_gate_open_setting = false;
    console_gate_durable = false;
    console_gate_open_armed = false;
    console_gate_proven = false;
    /* ITEM-463: the pointer's counters and cost window are boot state too. */
    console_pointer_moves = 0U;
    console_pointer_max_us = 0U;
    console_pointer_window_us = 0U;
    console_pointer_reported_moves = 0U;
    console_pointer_reported_max_us = 0U;
    console_pointer_publishing = false;
    /* ITEM-472: the window, the gain and the last published second as well. */
    console_pointer_window_dx = 0;
    console_pointer_window_dy = 0;
    console_pointer_window_dmax = 0U;
    console_pointer_gain = 1U;
    console_pointer_published_second = 0U;
    /* ITEM-480: the batch evidence is boot state too. */
    console_pointer_batches = 0U;
    console_pointer_batch_events = 0U;
    console_pointer_draw_us_total = 0U;
    console_pointer_sync_us_total = 0U;
    /*
     * ITEM-478 (round 16): the paint and service-line counters are boot state
     * too, so a host case reads the lines *it* painted and the service lines
     * *it* sent rather than an earlier case's.
     */
    console_paint_lines = 0U;
    console_paint_chars = 0U;
    console_paint_window_lines = 0U;
    console_paint_window_us = 0U;
    console_paint_max_us = 0U;
    console_paint_reported_lines = 0U;
    console_paint_reported_max_us = 0U;
    console_paint_publishing = false;
    console_service_lines = 0U;
}

void console_test_add_history(const char *command) {
    history_add(command);
}

const char *console_test_line(void) {
    return input_line;
}

size_t console_test_cursor(void) {
    return input_cursor;
}

/*
 * ITEM-457: the rows the body still holds below the cursor.  The next newline
 * scrolls when the cursor, advanced by one cell, would leave the screen, so
 * the count is how many cell-steps fit above that edge.
 */
uint64_t console_test_rows_until_scroll(void) {
    const uint64_t height = framebuffer_height();

    if (cursor_y + 2U * console_cell_height > height) {
        return 0U;
    }
    return (height - cursor_y - 2U * console_cell_height) / console_cell_height + 1U;
}

/*
 * ITEM-448: the rectangles a click would hit-test, so the host proof can aim
 * at a real painted line instead of guessing the geometry.
 */
size_t console_test_menu_target_count(void) {
    return pointer_target_count;
}

bool console_test_menu_target(size_t index, int32_t *x, int32_t *y,
                              int32_t *width, int32_t *height) {
    if (index >= pointer_target_count) {
        return false;
    }
    if (x != NULL) {
        *x = pointer_targets[index].x;
    }
    if (y != NULL) {
        *y = pointer_targets[index].y;
    }
    if (width != NULL) {
        *width = pointer_targets[index].width;
    }
    if (height != NULL) {
        *height = pointer_targets[index].height;
    }
    return true;
}
#endif
