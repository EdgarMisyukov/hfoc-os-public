/*
 * ITEM-419: the local menu model, proven on the host.
 *
 * The menu is what the person at the machine drives when there is no serial
 * port, no network and no panel: it has to open on something, move the way an
 * operator expects, run what is selected, and never let a keystroke aimed at it
 * reach the line editor underneath.  All of that is state and text, so all of
 * it is checked here - the framebuffer, the keyboard and the machine are only
 * the console's half.
 */
#include "local_menu.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition, message)                                          \
    do {                                                                   \
        ++checks;                                                          \
        if (!(condition)) {                                                \
            ++failures;                                                    \
            printf("FAIL: %s\n", (message));                               \
        }                                                                  \
    } while (0)

static struct keyboard_event event_buffer;

static const struct keyboard_event *character_event(char value) {
    memset(&event_buffer, 0, sizeof(event_buffer));
    event_buffer.type = KEYBOARD_EVENT_CHARACTER;
    event_buffer.character = value;
    return &event_buffer;
}

static const struct keyboard_event *type_event(enum keyboard_event_type type) {
    memset(&event_buffer, 0, sizeof(event_buffer));
    event_buffer.type = type;
    return &event_buffer;
}

static void test_open_and_move(void) {
    struct local_menu menu;

    memset(&menu, 0, sizeof(menu));
    CHECK(!local_menu_active(&menu),
          "a menu nobody opened is not active");
    local_menu_open(&menu);
    CHECK(local_menu_active(&menu), "the menu opens active");
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_STATUS,
          "the menu opens on Status, the entry an operator wants first");

    CHECK(local_menu_handle(&menu, type_event(KEYBOARD_EVENT_DOWN)) ==
              LOCAL_MENU_OUTCOME_REDRAW,
          "Down moves the selection and repaints");
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_NETWORK,
          "Down selects the next entry");
    CHECK(local_menu_handle(&menu, type_event(KEYBOARD_EVENT_UP)) ==
              LOCAL_MENU_OUTCOME_REDRAW,
          "Up moves the selection back");
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_STATUS,
          "Up returns to the entry above");

    /* The selection wraps: an operator holding Down never falls off the end. */
    CHECK(local_menu_handle(&menu, type_event(KEYBOARD_EVENT_UP)) ==
              LOCAL_MENU_OUTCOME_REDRAW,
          "Up at the first entry wraps");
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_RECOVERY,
          "wrapping upwards lands on the last entry");
    CHECK(local_menu_handle(&menu, type_event(KEYBOARD_EVENT_DOWN)) ==
              LOCAL_MENU_OUTCOME_REDRAW,
          "Down at the last entry wraps");
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_STATUS,
          "wrapping downwards lands on the first entry");
}

static void test_run_and_close(void) {
    struct local_menu menu;

    memset(&menu, 0, sizeof(menu));
    local_menu_open(&menu);
    local_menu_handle(&menu, type_event(KEYBOARD_EVENT_DOWN));
    local_menu_handle(&menu, type_event(KEYBOARD_EVENT_DOWN));
    local_menu_handle(&menu, type_event(KEYBOARD_EVENT_DOWN));
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_REBOOT,
          "three Downs reach Reboot");
    CHECK(local_menu_handle(&menu, type_event(KEYBOARD_EVENT_ENTER)) ==
              LOCAL_MENU_OUTCOME_RUN,
          "ENTER runs the selected entry");
    CHECK(strcmp(local_menu_entry_word(local_menu_selected(&menu)),
                 "reboot") == 0,
          "the entry the console runs is the one that was selected");

    /* A letter runs its own entry from anywhere in the menu. */
    local_menu_handle(&menu, character_event('s'));
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_STATUS &&
              local_menu_handle(&menu, character_event('s')) ==
                  LOCAL_MENU_OUTCOME_RUN,
          "a letter selects and runs the entry it names");
    local_menu_handle(&menu, character_event('3'));
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_DIAGNOSTICS,
          "a digit selects the entry of that number");
    CHECK(local_menu_handle(&menu, character_event('q')) ==
              LOCAL_MENU_OUTCOME_CLOSE,
          "Q closes the menu");
    local_menu_handle(&menu, character_event('q'));
    CHECK(local_menu_active(&menu),
          "closing is the console's decision, not the model's");

    local_menu_close(&menu);
    CHECK(!local_menu_active(&menu), "the console closes the menu");
    CHECK(local_menu_handle(&menu, type_event(KEYBOARD_EVENT_ENTER)) ==
              LOCAL_MENU_OUTCOME_NONE,
          "a closed menu consumes nothing");
}

static void test_unused_keys_stay_inside(void) {
    struct local_menu menu;

    memset(&menu, 0, sizeof(menu));
    local_menu_open(&menu);
    CHECK(local_menu_handle(&menu, character_event('z')) ==
              LOCAL_MENU_OUTCOME_NONE,
          "a key the menu does not use is consumed, not typed");
    CHECK(local_menu_handle(&menu, type_event(KEYBOARD_EVENT_LEFT)) ==
              LOCAL_MENU_OUTCOME_NONE,
          "the line editor's keys do not reach the line editor");
    CHECK(local_menu_selected(&menu) == LOCAL_MENU_ENTRY_STATUS,
          "an unused key does not move the selection");
    CHECK(local_menu_handle(NULL, character_event('r')) ==
              LOCAL_MENU_OUTCOME_NONE,
          "a null menu is refused");
    CHECK(local_menu_handle(&menu, NULL) == LOCAL_MENU_OUTCOME_NONE,
          "a null event is refused");
}

static void test_render(void) {
    struct local_menu menu;
    char rendered[LOCAL_MENU_RENDER_MAX + 1U];
    size_t length;

    memset(&menu, 0, sizeof(menu));
    local_menu_open(&menu);
    length = local_menu_render(&menu, APPLIANCE_LANGUAGE_EN, rendered,
                               sizeof(rendered));
    CHECK(length < sizeof(rendered), "the menu fits the console's buffer");
    CHECK(rendered[0] == '\n' && strstr(rendered, "LOCAL MENU") == rendered + 1,
          "the screen says what it is, on its own line");
    CHECK(strstr(rendered, "> [1] Status") != NULL,
          "the selected entry is marked");
    CHECK(strstr(rendered, "[4] Reboot") != NULL &&
              strstr(rendered, "[5] Shut down") != NULL,
          "the entries that power the machine are on the menu");
    CHECK(strstr(rendered, "UP/DOWN select, ENTER run, Q leave") != NULL,
          "the menu says how to drive it");

    local_menu_handle(&menu, type_event(KEYBOARD_EVENT_DOWN));
    length = local_menu_render(&menu, APPLIANCE_LANGUAGE_EN, rendered,
                               sizeof(rendered));
    CHECK(strstr(rendered, "> [2] Network") != NULL &&
              strstr(rendered, "> [1] Status") == NULL,
          "the marker follows the selection");

    /* A buffer too small is reported, never overrun. */
    memset(rendered, 'x', sizeof(rendered));
    length = local_menu_render(&menu, APPLIANCE_LANGUAGE_EN, rendered, 16U);
    CHECK(length >= 16U, "a short buffer reports the length it needed");
    CHECK(rendered[15] == '\0',
          "a short buffer is still terminated inside its capacity");
}

/*
 * ITEM-509 increment 9: the same menu in the chosen language.  The English
 * pass above stays byte-for-byte, because the catalog's English values are the
 * wording that stood here before; this pass proves the Russian path comes from
 * the catalog, that the machine words the console runs stay English, and that
 * the selection marker follows the selection in both languages.
 */
static void test_render_russian(void) {
    struct local_menu menu;
    char rendered[LOCAL_MENU_RENDER_MAX + 1U];
    size_t length;

    memset(&menu, 0, sizeof(menu));
    local_menu_open(&menu);
    length = local_menu_render(&menu, APPLIANCE_LANGUAGE_RU, rendered,
                               sizeof(rendered));
    CHECK(length < sizeof(rendered), "the Russian menu fits the buffer");
    CHECK(rendered[0] == '\n' && strstr(rendered, "ЛОКАЛЬНОЕ МЕНЮ") != NULL,
          "the screen says what it is in Russian");
    CHECK(strstr(rendered, "> [1] Статус") != NULL,
          "the selected entry is marked in Russian");
    CHECK(strstr(rendered, "[4] Перезагрузка") != NULL &&
              strstr(rendered, "[5] Выключение") != NULL &&
              strstr(rendered, "[6] Восстановление") != NULL,
          "every entry is translated");
    CHECK(strstr(rendered, "LOCAL MENU") == NULL &&
              strstr(rendered, "Status") == NULL,
          "no English label survives the Russian render");
    /*
     * The keys in brackets are what the operator presses and the words behind
     * them are what the console runs; both stay English by the item's boundary.
     */
    CHECK(strstr(rendered, "(s)") != NULL && strstr(rendered, "(p)") != NULL,
          "the keys in brackets stay English in the Russian menu");
    CHECK(strcmp(local_menu_entry_word(LOCAL_MENU_ENTRY_SHUTDOWN),
                 "shutdown") == 0,
          "the word the console runs stays English at every language");
    CHECK(strstr(rendered, "ВВЕРХ/ВНИЗ") != NULL &&
              strstr(rendered, "ENTER") != NULL,
          "the Russian menu says how to drive it");

    local_menu_handle(&menu, type_event(KEYBOARD_EVENT_DOWN));
    length = local_menu_render(&menu, APPLIANCE_LANGUAGE_RU, rendered,
                               sizeof(rendered));
    CHECK(length < sizeof(rendered) &&
              strstr(rendered, "> [2] Сеть") != NULL &&
              strstr(rendered, "> [1] Статус") == NULL,
          "the marker follows the selection in Russian");

    CHECK(strcmp(local_menu_entry_label(LOCAL_MENU_ENTRY_SHUTDOWN,
                                        APPLIANCE_LANGUAGE_RU),
                 "Выключение") == 0,
          "the label lookup answers in the requested language");
    CHECK(strcmp(local_menu_entry_label((enum local_menu_entry)99,
                                        APPLIANCE_LANGUAGE_RU),
                 "НЕИЗВЕСТНО") == 0,
          "an entry outside the menu is named as unknown in Russian");
}

static void test_every_entry_named(void) {
    size_t entry;

    for (entry = 0U; entry < (size_t)LOCAL_MENU_ENTRY_COUNT; ++entry) {
        const char *label = local_menu_entry_label(
            (enum local_menu_entry)entry, APPLIANCE_LANGUAGE_EN);
        const char *word = local_menu_entry_word((enum local_menu_entry)entry);

        CHECK(label != NULL && label[0] != '\0',
              "every entry has a label");
        CHECK(word != NULL && word[0] != '\0',
              "every entry has the word the console runs");
        CHECK(strcmp(label, "UNKNOWN") != 0,
              "every entry is named, not defaulted");
    }
    CHECK(strcmp(local_menu_entry_label((enum local_menu_entry)99,
                                        APPLIANCE_LANGUAGE_EN),
                 "UNKNOWN") == 0,
          "an entry outside the menu is named as unknown");
}

/*
 * ITEM-455: the keys the menu answers to, published as one list.
 *
 * The owner's run had no `[LOCAL-MENU]` record at all, and the offer it would
 * have printed named only ENTER: the key that opens the network editor - the
 * one place the static address is set - was visible only after opening the
 * menu.  The record now carries the list, and it comes from this table, so a
 * menu that gains or renames a key changes the record with it.
 */
static void test_key_list(void) {
    char list[LOCAL_MENU_KEY_LIST_MAX + 1U];
    char exact[LOCAL_MENU_KEY_LIST_MAX + 1U];
    char small[4];

    memset(list, 0x7F, sizeof(list));
    CHECK(local_menu_key_list(list, sizeof(list)) == strlen(list),
          "the key list reports its own length");
    CHECK(strcmp(list, "s,n,d,r,p,y") == 0,
          "the key list is the menu's own keys, in its own order");
    CHECK(strstr(list, "n") != NULL,
          "the network key is in the list the record publishes");
    /* Every key in the list is one the menu actually handles. */
    for (size_t index = 0U; index < (size_t)LOCAL_MENU_ENTRY_COUNT; ++index) {
        const char *word = local_menu_entry_word((enum local_menu_entry)index);
        CHECK(word != NULL && strchr(list, word[0]) != NULL,
              "every entry's key appears in the published list");
    }
    /* The list is written whole or not at all. */
    memset(small, 0x7F, sizeof(small));
    CHECK(local_menu_key_list(small, sizeof(small)) == 0U,
          "a buffer too small for the list writes nothing");
    CHECK(small[0] == '\0', "and leaves a valid empty string");
    CHECK(local_menu_key_list(NULL, sizeof(list)) == 0U,
          "a missing buffer is refused");
    CHECK(local_menu_key_list(exact, 0U) == 0U,
          "a zero-sized buffer is refused");
    CHECK(local_menu_key_list(exact, strlen("s,n,d,r,p,y") + 1U) ==
              strlen("s,n,d,r,p,y"),
          "a buffer that holds exactly the list and its terminator is enough");
    CHECK(strcmp(exact, "s,n,d,r,p,y") == 0,
          "and carries the whole list");
}

/*
 * ITEM-430: the offer the machine makes when nobody asks for the menu.
 *
 * The menu is reachable by typing `menu`, which assumes an operator who knows
 * the word and a keyboard to type it on; on a machine with no local input at
 * all it is unreachable in principle.  The decision to offer it is state and
 * time, so it is checked here: once per boot, after the idle window, never
 * after a keystroke, never while a menu is already open, and at once - without
 * a window - on a machine that cannot type.
 */
static void test_offer_decision(void) {
    struct local_menu_offer offer;

    memset(&offer, 0, sizeof(offer));
    CHECK(local_menu_offer_due(&offer, 1000U, false, true, false, false) ==
              LOCAL_MENU_OFFER_NONE,
          "no offer before the console is ready");
    CHECK(local_menu_offer_due(&offer, 1000U, true, true, false, false) ==
              LOCAL_MENU_OFFER_NONE,
          "no offer at the moment the console becomes ready");
    CHECK(local_menu_offer_due(
              &offer, 1000U + LOCAL_MENU_OFFER_MS - 1U, true, true, false,
              false) == LOCAL_MENU_OFFER_NONE,
          "no offer one millisecond before the idle window closes");
    CHECK(local_menu_offer_due(
              &offer, 1000U + LOCAL_MENU_OFFER_MS, true, true, false,
              false) == LOCAL_MENU_OFFER_IDLE,
          "the offer comes after the idle window");
    CHECK(local_menu_offer_due(
              &offer, 1000U + LOCAL_MENU_OFFER_MS + 5000U, true, true, false,
              false) == LOCAL_MENU_OFFER_NONE,
          "the offer is made once per boot");

    /*
     * The window has to be open before the two suppression gates mean
     * anything: a first call returns NONE whatever the gates say, so these
     * cases start the window on their own first turn.
     */
    memset(&offer, 0, sizeof(offer));
    CHECK(local_menu_offer_due(&offer, 10U, true, true, false, false) ==
              LOCAL_MENU_OFFER_NONE,
          "the suppression fixture opens the idle window");
    CHECK(local_menu_offer_due(
              &offer, 10U + LOCAL_MENU_OFFER_MS, true, true, true,
              false) == LOCAL_MENU_OFFER_NONE,
          "an operator who has pressed a key is not offered a menu");
    memset(&offer, 0, sizeof(offer));
    CHECK(local_menu_offer_due(&offer, 10U, true, true, false, false) ==
              LOCAL_MENU_OFFER_NONE,
          "the open-menu fixture opens the idle window");
    CHECK(local_menu_offer_due(
              &offer, 10U + LOCAL_MENU_OFFER_MS, true, true, false,
              true) == LOCAL_MENU_OFFER_NONE,
          "an open menu is not offered again");
    memset(&offer, 0, sizeof(offer));
    CHECK(local_menu_offer_due(&offer, 10U, true, false, false, false) ==
              LOCAL_MENU_OFFER_NO_LOCAL_INPUT,
          "a machine that cannot type is offered the menu at once");
    CHECK(local_menu_offer_due(&offer, 10U, true, false, false, false) ==
              LOCAL_MENU_OFFER_NONE,
          "the machine without input is offered the menu once");
}

int main(void) {
    test_open_and_move();
    test_run_and_close();
    test_unused_keys_stay_inside();
    test_render();
    test_render_russian();
    test_every_entry_named();
    test_key_list();
    test_offer_decision();
    printf("local menu host test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
