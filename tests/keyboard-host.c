#include "keyboard.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef HFOC_KEYBOARD_HOST_TEST
void keyboard_test_reset(void);
#endif

static uint8_t mock_status;
static uint8_t mock_data;
static unsigned status_reads;
static unsigned data_reads;
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

static void reset_mock_state(void) {
    mock_status = 0U;
    mock_data = 0U;
    status_reads = 0U;
    data_reads = 0U;
}

static void feed_scancode(uint8_t scancode) {
    mock_status = 0x01U;
    mock_data = scancode;
    keyboard_handle_irq();

    mock_status = 0x00U;
    keyboard_handle_irq();
}

uint8_t inb(uint16_t port) {
    if (port == 0x64U) {
        ++status_reads;
        return mock_status;
    }
    if (port == 0x60U) {
        ++data_reads;
        return mock_data;
    }
    return 0U;
}

void outb(uint16_t port, uint8_t value) {
    (void)port;
    (void)value;
}

static void test_empty_duplicate(void) {
    reset_mock_state();
    keyboard_test_reset();

    mock_status = 0x01U;
    mock_data = 0x1eU;
    keyboard_handle_irq();

    mock_status = 0x00U;
    keyboard_handle_irq();

    CHECK(data_reads == 1U,
          "empty duplicate IRQ reads data port only for real output");
    CHECK(status_reads == 2U,
          "empty duplicate IRQ reads status for both IRQs");
    CHECK(keyboard_irq_count() == 1U,
          "empty duplicate IRQ does not increment count");
}

static void test_make_break_migration(void) {
    reset_mock_state();
    keyboard_test_reset();

    static const uint8_t makes[3] = { 0x1eU, 0x30U, 0x2eU };
    static const uint8_t breaks[3] = { 0x9eU, 0xb0U, 0xaeU };
    for (unsigned int i = 0U; i < 3U; ++i) {
        mock_status = 0x01U;
        mock_data = makes[i];
        keyboard_handle_irq();

        mock_status = 0x00U;
        keyboard_handle_irq();

        mock_status = 0x01U;
        mock_data = breaks[i];
        keyboard_handle_irq();

        mock_status = 0x00U;
        keyboard_handle_irq();
    }

    struct keyboard_event event;
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER && event.character == 'a',
          "first decoded character is a");
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER && event.character == 'b',
          "second decoded character is b");
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER && event.character == 'c',
          "third decoded character is c");
    CHECK(!keyboard_pop_event(&event),
          "no extra decoded character after abc");
    CHECK(data_reads == 6U,
          "six scancode bytes read, one per make/break byte");
    CHECK(keyboard_irq_count() == 6U,
          "six real output-buffer IRQs processed");
}

static void test_e0_events(void) {
    reset_mock_state();
    keyboard_test_reset();

    static const struct {
        uint8_t scancode;
        enum keyboard_event_type type;
        const char *name;
    } cases[] = {
        { 0x4bU, KEYBOARD_EVENT_LEFT, "E0 4B Left" },
        { 0x4dU, KEYBOARD_EVENT_RIGHT, "E0 4D Right" },
        { 0x47U, KEYBOARD_EVENT_HOME, "E0 47 Home" },
        { 0x4fU, KEYBOARD_EVENT_END, "E0 4F End" },
        { 0x53U, KEYBOARD_EVENT_DELETE, "E0 53 Delete" },
    };

    for (unsigned int i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        feed_scancode(0xe0U);
        feed_scancode(cases[i].scancode);
        feed_scancode(0xe0U);
        feed_scancode((uint8_t)(cases[i].scancode | 0x80U));

        struct keyboard_event event;
        CHECK(keyboard_pop_event(&event) && event.type == cases[i].type,
              cases[i].name);
        CHECK(!keyboard_pop_event(&event),
              "E0 break emits no duplicate event");
    }
}

static void test_history_arrow_events(void) {
    reset_mock_state();
    keyboard_test_reset();

    feed_scancode(0xe0U);
    feed_scancode(0x48U);
    feed_scancode(0xe0U);
    feed_scancode(0xc8U);

    struct keyboard_event event;
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_UP,
          "E0 48 make emits UP event");
    CHECK(!keyboard_pop_event(&event),
          "E0 C8 break emits no duplicate event");

    feed_scancode(0xe0U);
    feed_scancode(0x50U);
    feed_scancode(0xe0U);
    feed_scancode(0xd0U);

    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_DOWN,
          "E0 50 make emits DOWN event");
    CHECK(!keyboard_pop_event(&event),
          "E0 D0 break emits no duplicate event");
}

static void test_enter_and_backspace_events(void) {
    reset_mock_state();
    keyboard_test_reset();

    feed_scancode(0x1cU);
    feed_scancode(0x0eU);

    struct keyboard_event event;
    CHECK(keyboard_pop_event(&event) && event.type == KEYBOARD_EVENT_ENTER,
          "Enter make emits ENTER event");
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_BACKSPACE,
          "Backspace make emits BACKSPACE event");
    CHECK(!keyboard_pop_event(&event),
          "Enter/Backspace emit no extra events");
}

static void test_shift_caps_lock_and_shifted_digit(void) {
    reset_mock_state();
    keyboard_test_reset();

    /* Shift + a -> A. */
    feed_scancode(0x2aU);
    feed_scancode(0x1eU);
    feed_scancode(0xaaU);

    /* CapsLock + a -> A. */
    feed_scancode(0x3aU);
    feed_scancode(0x1eU);

    /* Shift + CapsLock + a -> a. */
    feed_scancode(0x2aU);
    feed_scancode(0x1eU);
    feed_scancode(0xaaU);

    /* Shift + 1 -> !. */
    feed_scancode(0x2aU);
    feed_scancode(0x02U);
    feed_scancode(0xaaU);

    struct keyboard_event event;
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER && event.character == 'A',
          "Shift XOR CapsLock maps a to A with Shift");
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER && event.character == 'A',
          "CapsLock maps a to A");
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER && event.character == 'a',
          "Shift XOR CapsLock maps a back to a");
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER && event.character == '!',
          "Shifted 1 maps to !");
    CHECK(!keyboard_pop_event(&event),
          "Shift/CapsLock emits no extra events");
}

static void test_unsupported_e0_is_consumed(void) {
    reset_mock_state();
    keyboard_test_reset();

    feed_scancode(0xe0U);
    feed_scancode(0x1eU);
    feed_scancode(0xe0U);
    feed_scancode(0x9eU);
    feed_scancode(0x1eU);

    struct keyboard_event event;
    CHECK(keyboard_pop_event(&event) &&
              event.type == KEYBOARD_EVENT_CHARACTER && event.character == 'a',
          "unsupported E0 make is consumed without a character");
    CHECK(!keyboard_pop_event(&event),
          "unsupported E0 break is consumed without a character");
}

int main(void) {
    test_empty_duplicate();
    test_make_break_migration();
    test_e0_events();
    test_history_arrow_events();
    test_enter_and_backspace_events();
    test_shift_caps_lock_and_shifted_digit();
    test_unsupported_e0_is_consumed();

    if (failures != 0) {
        printf("Keyboard migration host tests: FAIL (%d)\n", failures);
        return 1;
    }
    printf("Keyboard migration host tests: OK\n");
    return 0;
}
