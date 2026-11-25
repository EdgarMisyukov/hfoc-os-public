/*
 * ITEM-392 PS/2 degraded-boot host proof.
 *
 * The production kernel used to treat a failed keyboard_init() as fatal, so a
 * machine with no i8042 - the hardware acceptance host found on 2026-09-12 -
 * halted with "PS/2 keyboard initialisation failed" before the appliance was
 * usable. QEMU always emulates a PS/2 controller, so the CI proof alone cannot
 * say *which* failure the driver saw; this proof drives keyboard_init() over a
 * mock controller and asserts that
 *
 *   - each hardware failure is classified and named (absent controller, stuck
 *     controller, rejected configuration, absent keyboard);
 *   - every classified failure is survivable, i.e. the boot policy may
 *     continue and publish it as the reason local input is unavailable;
 *   - only an unclassified status is not survivable, so the one keyboard
 *     failure that stops the boot is the one that cannot be described;
 *   - the configuration the kernel wrote is read back before the keyboard is
 *     reported usable, because an unverified translation bit would decode
 *     scan-code set 2 with the set-1 tables;
 *   - the interrupt path stays safe after a failed initialisation, since IRQ1
 *     remains routed to its IOAPIC vector.
 */
#ifdef HFOC_KEYBOARD_INIT_HOST_TEST

#include "keyboard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define PS2_DATA_PORT 0x60U
#define PS2_STATUS_PORT 0x64U

#define PS2_STATUS_OUTPUT 0x01U
#define PS2_STATUS_INPUT 0x02U
#define PS2_STATUS_AUX 0x20U

#define PS2_CMD_READ_CONFIG 0x20U
#define PS2_CMD_WRITE_CONFIG 0x60U
#define PS2_CMD_DISABLE_PORT2 0xa7U
#define PS2_CMD_DISABLE_PORT1 0xadU
#define PS2_CMD_ENABLE_PORT1 0xaeU
#define PS2_CFG_IRQ1 0x01U
#define PS2_CFG_PORT1_CLOCK 0x10U
#define PS2_CFG_TRANSLATION 0x40U
#define KBD_ACK 0xfaU

#define MOCK_QUEUE 8U

/* Mock i8042 state. */
static uint8_t status_idle;
static uint8_t output_queue[MOCK_QUEUE];
static unsigned int output_head;
static unsigned int output_tail;
static uint8_t config_register;
static bool config_write_pending;
static bool config_write_honoured;
static bool controller_answers;
static bool keyboard_acks;
static unsigned int controller_commands;
static unsigned int config_writes;
static unsigned int keyboard_commands;

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

static void mock_reset(void) {
    status_idle = 0x14U;
    output_head = 0U;
    output_tail = 0U;
    config_register = 0x14U;
    config_write_pending = false;
    config_write_honoured = true;
    controller_answers = true;
    keyboard_acks = true;
    controller_commands = 0U;
    config_writes = 0U;
    keyboard_commands = 0U;
}

static void mock_push(uint8_t value) {
    if (output_tail - output_head >= MOCK_QUEUE) {
        return;
    }
    output_queue[output_tail % MOCK_QUEUE] = value;
    ++output_tail;
}

uint8_t inb(uint16_t port) {
    if (port == PS2_STATUS_PORT) {
        return output_head != output_tail
                   ? (uint8_t)(status_idle | PS2_STATUS_OUTPUT)
                   : status_idle;
    }
    if (port == PS2_DATA_PORT) {
        uint8_t value;

        if (output_head == output_tail) {
            return 0xffU;
        }
        value = output_queue[output_head % MOCK_QUEUE];
        ++output_head;
        return value;
    }
    return 0xffU;
}

void outb(uint16_t port, uint8_t value) {
    if (port == PS2_STATUS_PORT) {
        ++controller_commands;
        switch (value) {
        case PS2_CMD_DISABLE_PORT1:
        case PS2_CMD_DISABLE_PORT2:
        case PS2_CMD_ENABLE_PORT1:
            break;
        case PS2_CMD_READ_CONFIG:
            if (controller_answers) {
                mock_push(config_register);
            }
            break;
        case PS2_CMD_WRITE_CONFIG:
            config_write_pending = true;
            break;
        default:
            break;
        }
        return;
    }
    if (port == PS2_DATA_PORT) {
        if (config_write_pending) {
            config_write_pending = false;
            ++config_writes;
            if (config_write_honoured) {
                config_register = value;
            }
            return;
        }
        ++keyboard_commands;
        if (keyboard_acks) {
            mock_push(KBD_ACK);
        }
    }
}

static void begin_case(void) {
    /*
     * keyboard_init() resets every driver-side buffer and flag, so the case
     * only has to put the mock controller back into its default shape.
     */
    mock_reset();
}

static void test_status_mapping(void) {
    CHECK(keyboard_init_status_string(KEYBOARD_INIT_OK) != NULL &&
              keyboard_init_status_string(KEYBOARD_INIT_OK)[0] == 'O',
          "the usable status is named OK");
    CHECK(keyboard_init_status_string(
              KEYBOARD_INIT_CONTROLLER_ABSENT) != NULL &&
              keyboard_init_status_string(
                  KEYBOARD_INIT_CONTROLLER_ABSENT)[0] == 'C',
          "an absent controller has a name");
    CHECK(keyboard_init_status_string(
              KEYBOARD_INIT_CONTROLLER_STUCK) != NULL,
          "a stuck controller has a name");
    CHECK(keyboard_init_status_string(
              KEYBOARD_INIT_CONFIG_REJECTED) != NULL,
          "a rejected configuration has a name");
    CHECK(keyboard_init_status_string(
              KEYBOARD_INIT_KEYBOARD_ABSENT) != NULL,
          "an absent keyboard has a name");
    CHECK(keyboard_init_status_string(
              KEYBOARD_INIT_STATUS_UNKNOWN) != NULL,
          "the unclassified status has a name");
    CHECK(keyboard_init_status_string(
              (enum keyboard_init_status)0x7fff) != NULL,
          "a value outside the declared set still renders");
}

static void test_boot_policy(void) {
    CHECK(keyboard_init_status_boots(KEYBOARD_INIT_OK),
          "a usable keyboard boots");
    CHECK(keyboard_init_status_boots(KEYBOARD_INIT_CONTROLLER_ABSENT),
          "an absent controller boots without local input");
    CHECK(keyboard_init_status_boots(KEYBOARD_INIT_CONTROLLER_STUCK),
          "a stuck controller boots without local input");
    CHECK(keyboard_init_status_boots(KEYBOARD_INIT_CONFIG_REJECTED),
          "a rejected configuration boots without local input");
    CHECK(keyboard_init_status_boots(KEYBOARD_INIT_KEYBOARD_ABSENT),
          "an absent keyboard boots without local input");
    CHECK(!keyboard_init_status_boots(KEYBOARD_INIT_STATUS_UNKNOWN),
          "only an unclassified keyboard status stops the boot");
    CHECK(!keyboard_init_status_boots(
              (enum keyboard_init_status)0x7fff),
          "a value outside the declared set stops the boot");
}

static void test_controller_absent(void) {
    struct keyboard_event event;
    const struct keyboard_init_diagnostic *diagnostic;

    begin_case();
    /* All ones on the status port: nothing drives the port at all. */
    status_idle = 0xffU;

    CHECK(keyboard_init() == KEYBOARD_INIT_CONTROLLER_ABSENT,
          "an all-ones status port is classified as an absent controller");
    diagnostic = keyboard_init_diagnostic();
    CHECK(diagnostic != NULL, "the diagnostic is always available");
    CHECK(diagnostic->status == KEYBOARD_INIT_CONTROLLER_ABSENT,
          "the diagnostic carries the absent controller status");
    CHECK(diagnostic->status_port == 0xffU,
          "the diagnostic carries the all-ones status byte");
    CHECK(diagnostic->controller_config == 0U,
          "no configuration byte was read");
    CHECK(keyboard_controller_config() == 0U,
          "the published configuration is empty");

    /* IRQ1 stays routed: a spurious interrupt must not decode anything. */
    for (unsigned int index = 0U; index < 8U; ++index) {
        keyboard_handle_irq();
    }
    CHECK(keyboard_irq_count() == 0U,
          "a spurious IRQ1 decodes no scan code");
    CHECK(!keyboard_pop_event(&event),
          "no keyboard event is produced without a controller");
}

static void test_controller_input_stuck(void) {
    begin_case();
    /* The input buffer never drains, but the port is not all ones. */
    status_idle = PS2_STATUS_INPUT;

    CHECK(keyboard_init() == KEYBOARD_INIT_CONTROLLER_STUCK,
          "a status port that never drains is a stuck controller");
    CHECK(keyboard_init_diagnostic()->status_port == PS2_STATUS_INPUT,
          "the diagnostic carries the stuck status byte");
}

static void test_controller_output_stuck(void) {
    begin_case();
    /* Commands are accepted; the controller never answers one. */
    status_idle = 0x00U;
    controller_answers = false;

    CHECK(keyboard_init() == KEYBOARD_INIT_CONTROLLER_STUCK,
          "a controller that never answers is stuck");
    CHECK(keyboard_init_diagnostic()->status_port == 0x00U,
          "the diagnostic carries the silent status byte");
}

static void test_config_rejected(void) {
    begin_case();
    /* The controller keeps the configuration it had: translation stays off. */
    config_write_honoured = false;

    CHECK(keyboard_init() == KEYBOARD_INIT_CONFIG_REJECTED,
          "an unverified configuration is rejected");
    CHECK(config_writes != 0U,
          "the kernel really wrote a configuration byte");
    CHECK(keyboard_init_diagnostic()->controller_config == 0x14U,
          "the diagnostic carries the configuration read back");
    CHECK(keyboard_commands == 0U,
          "no scan-code command is sent to a mistranslating controller");
}

static void test_keyboard_absent(void) {
    begin_case();
    keyboard_acks = false;

    CHECK(keyboard_init() == KEYBOARD_INIT_KEYBOARD_ABSENT,
          "a keyboard that never acknowledges is classified as absent");
    CHECK(keyboard_commands != 0U,
          "the kernel really asked the keyboard for scan-code set 2");
    CHECK((keyboard_init_diagnostic()->controller_config &
           (PS2_CFG_IRQ1 | PS2_CFG_TRANSLATION | PS2_CFG_PORT1_CLOCK)) ==
              (PS2_CFG_IRQ1 | PS2_CFG_TRANSLATION),
          "the controller itself stayed configured for set-1 translation");
}

static void test_success_reads_configuration_back(void) {
    begin_case();
    config_register = 0x14U;

    CHECK(keyboard_init() == KEYBOARD_INIT_OK,
          "a working controller and keyboard initialise");
    CHECK(controller_commands >= 5U,
          "the controller received the full configuration sequence");
    CHECK(config_writes == 1U,
          "exactly one configuration byte was written");
    CHECK(keyboard_commands == 4U,
          "the keyboard received disable/select/enable scan commands");
    CHECK((keyboard_controller_config() &
           (PS2_CFG_IRQ1 | PS2_CFG_TRANSLATION | PS2_CFG_PORT1_CLOCK)) ==
              (PS2_CFG_IRQ1 | PS2_CFG_TRANSLATION),
          "the configuration read back enables IRQ1 and translation");
    CHECK(keyboard_init_diagnostic()->status == KEYBOARD_INIT_OK,
          "the diagnostic carries the usable status");
    CHECK(keyboard_init_status_boots(KEYBOARD_INIT_OK),
          "the usable status boots");
}

static void test_scan_codes_decode_after_success(void) {
    struct keyboard_event event;

    begin_case();
    CHECK(keyboard_init() == KEYBOARD_INIT_OK,
          "the keyboard initialises before a scan code is fed");

    /* 'a' make, fed exactly as the IRQ path reads it. */
    mock_push(0x1eU);
    keyboard_handle_irq();

    CHECK(keyboard_irq_count() == 1U, "the scan code is counted");
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER &&
              event.character == 'a',
          "the initialised keyboard still decodes set-1 scan codes");
    CHECK(!keyboard_pop_event(&event), "no extra event is produced");
}

int main(void) {
    test_status_mapping();
    test_boot_policy();
    test_controller_absent();
    test_controller_input_stuck();
    test_controller_output_stuck();
    test_config_rejected();
    test_keyboard_absent();
    test_success_reads_configuration_back();
    test_scan_codes_decode_after_success();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("PS/2 degraded-boot host test: ALL OK");
    return 0;
}

#else
#error "tests/keyboard-init-host.c requires -DHFOC_KEYBOARD_INIT_HOST_TEST"
#endif
