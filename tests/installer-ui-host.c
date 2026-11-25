/*
 * ITEM-488: the installer UI foundation, proven on the host.
 *
 * The UI is a model plus one renderer, and the renderer draws through the
 * display abstraction - so the same screen has to come out of a machine with a
 * GOP framebuffer and out of a machine whose only output is text.  This proof
 * pins both readings, the navigation, and the rule that keeps the screen
 * honest: capabilities this item does not implement say `NOT_IMPLEMENTED`, and
 * no row is ever `AVAILABLE` unless something measured it as such.
 */
#include "installer_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static char lines[INSTALLER_UI_LINES_MAX][INSTALLER_UI_LINE_BYTES];

static bool screen_has(const char *needle)
{
    size_t index;

    for (index = 0U; index < INSTALLER_UI_LINES_MAX; ++index) {
        if (strcmp(lines[index], needle) == 0) {
            return true;
        }
    }
    return false;
}

static struct display_framebuffer ui_fixture(uint64_t width, uint64_t height)
{
    struct display_framebuffer framebuffer;

    memset(&framebuffer, 0, sizeof(framebuffer));
    framebuffer.present = true;
    framebuffer.address = (void *)UINT64_C(0x00000000FD000000);
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.pitch = width * 4U;
    framebuffer.bpp = 32U;
    framebuffer.memory_model = 1U;
    framebuffer.red_mask_size = 8U;
    framebuffer.red_mask_shift = 16U;
    framebuffer.green_mask_size = 8U;
    framebuffer.green_mask_shift = 8U;
    framebuffer.blue_mask_size = 8U;
    framebuffer.blue_mask_shift = 0U;
    return framebuffer;
}

static void test_foundation_on_a_gop_machine(void)
{
    struct display_framebuffer framebuffer = ui_fixture(1920U, 1080U);
    struct installer_ui_model model;
    size_t count;

    display_reset();
    CHECK(display_probe_framebuffer(&framebuffer) == DISPLAY_PROBE_OK,
          "the fixture machine has a firmware framebuffer");
    installer_ui_foundation(&model, APPLIANCE_LANGUAGE_DEFAULT);
    CHECK(model.row_count == INSTALLER_UI_ROWS &&
              strcmp(model.rows[0].label, "Display") == 0 &&
              model.rows[0].state == INSTALLER_UI_STATE_AVAILABLE &&
              strcmp(model.rows[0].value, "GOP 1920x1080 BGRX8888") == 0,
          "the Display row carries the backend, the geometry and the format "
          "the abstraction measured");
    CHECK(model.rows[1].state == INSTALLER_UI_STATE_NOT_IMPLEMENTED &&
              model.rows[2].state == INSTALLER_UI_STATE_NOT_IMPLEMENTED,
          "storage and network are NOT_IMPLEMENTED, not a fake pass");
    count = installer_ui_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(count > 0U && count <= INSTALLER_UI_LINES_MAX,
          "the screen fits the bounded line count");
    CHECK(screen_has("HFOC Installer") && screen_has("> Display") &&
              screen_has("  Backend: GOP") &&
              screen_has("  Resolution: 1920x1080") &&
              screen_has("  Pixel format: BGRX8888") &&
              screen_has("  Status: AVAILABLE") &&
              screen_has("  Fallback: TEXT") && screen_has("  Storage") &&
              screen_has("  Status: NOT_IMPLEMENTED") && screen_has("  Network") &&
              screen_has("  Continue"),
          "the framebuffer screen names the display reading and the "
          "capabilities the next items own");
}

static void test_text_fallback_screen(void)
{
    struct installer_ui_model model;
    size_t count;

    display_reset();
    (void)display_probe_framebuffer(NULL);
    display_bind_text_fallback("NO_FRAMEBUFFER", true);
    installer_ui_foundation(&model, APPLIANCE_LANGUAGE_DEFAULT);
    CHECK(model.rows[0].state == INSTALLER_UI_STATE_UNAVAILABLE &&
              strcmp(model.rows[0].value, "TEXT NONE UNKNOWN") == 0,
          "without a framebuffer the Display row says TEXT/NONE/UNKNOWN");
    CHECK(model.rows[1].state == INSTALLER_UI_STATE_NOT_IMPLEMENTED &&
              model.rows[2].state == INSTALLER_UI_STATE_NOT_IMPLEMENTED,
          "the text screen carries the same capability states");
    count = installer_ui_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(count > 0U && count <= INSTALLER_UI_LINES_MAX,
          "the text screen fits the bounded line count");
    CHECK(screen_has("  Backend: TEXT") && screen_has("  Resolution: NONE") &&
              screen_has("  Status: UNAVAILABLE") &&
              screen_has("  Fallback: SERIAL") && screen_has("  Storage") &&
              screen_has("  Network") && screen_has("  Continue"),
          "the same flow is readable with no framebuffer, and the serial "
          "stream is named when it carries it");
}

static void test_degraded_display_is_not_available(void)
{
    struct display_framebuffer framebuffer = ui_fixture(1024U, 768U);
    struct installer_ui_model model;

    framebuffer.pitch = 16U;
    display_reset();
    CHECK(display_probe_framebuffer(&framebuffer) == DISPLAY_PROBE_INVALID_PITCH,
          "a malformed framebuffer is refused by the abstraction");
    installer_ui_foundation(&model, APPLIANCE_LANGUAGE_DEFAULT);
    CHECK(model.rows[0].state != INSTALLER_UI_STATE_AVAILABLE,
          "a refused framebuffer never becomes an AVAILABLE display row");
    (void)installer_ui_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(screen_has("  Status: UNAVAILABLE") &&
              screen_has("  Fallback: TEXT"),
          "the screen shows the unavailable status and the text fallback");
}

static void test_navigation_and_selection(void)
{
    struct installer_ui_model model;
    char record[128U];

    display_reset();
    installer_ui_foundation(&model, APPLIANCE_LANGUAGE_DEFAULT);
    CHECK(model.selected == 0U && !installer_ui_activate(&model),
          "the screen opens on the Display row, which does not continue");
    CHECK(installer_ui_move(&model, -1) == false && model.selected == 0U,
          "the selection stops at the first row");
    while (installer_ui_move(&model, 1)) {
    }
    CHECK(model.selected == INSTALLER_UI_ROWS - 1U &&
              installer_ui_activate(&model),
          "moving down reaches Continue, and only it continues");
    CHECK(installer_ui_move(&model, 1) == false,
          "the selection stops at the last row");
    CHECK(installer_ui_record(&model, record, sizeof(record)) > 0U &&
              strcmp(record,
                     "[INSTALLER-UI] selected=Continue state=AVAILABLE") == 0,
          "the selected row is publishable as one bounded record");
    CHECK(strcmp(installer_ui_key_name('w'), "UP") == 0 &&
              strcmp(installer_ui_key_name('s'), "DOWN") == 0 &&
              strcmp(installer_ui_key_name('\r'), "ENTER") == 0 &&
              strcmp(installer_ui_key_name('x'), "OTHER") == 0,
          "the keys the screen answers have one spelling each");
    CHECK(installer_ui_record(NULL, record, sizeof(record)) == 0U &&
              installer_ui_record(&model, record, 8U) == 0U,
          "a record that cannot fit is refused, never truncated");
    CHECK(installer_ui_lines(NULL, APPLIANCE_LANGUAGE_DEFAULT, lines) == 0U,
          "a screen without a model renders nothing");
}

/*
 * ITEM-509 increment 14: the foundation screen reads the one catalog in the
 * language the caller names.  The English screen above is byte for byte what it
 * was; the Russian one comes from the catalog column, keeps the machine state
 * words (AVAILABLE, NOT_IMPLEMENTED, GOP, TEXT, NONE) and carries no English
 * prose; a language the catalog cannot answer is a named refusal, never half a
 * screen.
 */
static void test_russian_foundation_screen(void)
{
    struct installer_ui_model model;
    char record[128U];
    size_t count;

    display_reset();
    (void)display_probe_framebuffer(NULL);
    display_bind_text_fallback("NO_FRAMEBUFFER", true);
    installer_ui_foundation(&model, APPLIANCE_LANGUAGE_RU);
    CHECK(!model.missing_translation &&
              strcmp(model.title, "Установщик HFOC") == 0 &&
              strcmp(model.rows[0].label, "Дисплей") == 0 &&
              strcmp(model.rows[1].label, "Хранилище") == 0 &&
              strcmp(model.rows[2].label, "Сеть") == 0 &&
              strcmp(model.rows[3].label, "Продолжить") == 0 &&
              strcmp(model.rows[1].value, "поиск дисков и разметка") == 0 &&
              strcmp(model.rows[3].value, "только диагностический поток") == 0,
          "ITEM-509: the foundation model reads the Russian catalog column");
    count = installer_ui_lines(&model, APPLIANCE_LANGUAGE_RU, lines);
    CHECK(count > 0U && count <= INSTALLER_UI_LINES_MAX &&
              screen_has("Установщик HFOC") && screen_has("> Дисплей") &&
              screen_has("  Интерфейс: TEXT") &&
              screen_has("  Разрешение: NONE") &&
              screen_has("  Состояние: UNAVAILABLE") &&
              screen_has("  Резерв: SERIAL") && screen_has("  Хранилище") &&
              screen_has("  Сеть") && screen_has("  Продолжить"),
          "ITEM-509: the Russian screen comes from the catalog and keeps the "
          "machine state words");
    CHECK(!screen_has("HFOC Installer") && !screen_has("  Backend: TEXT") &&
              !screen_has("  Storage") && !screen_has("  Continue"),
          "ITEM-509: the Russian screen carries no English interface prose");
    /*
     * The record is a machine line the acceptance logs quote, so it stays
     * English whatever language the screen is drawn in.
     */
    CHECK(installer_ui_record(&model, record, sizeof(record)) > 0U &&
              strcmp(record, "[INSTALLER-UI] selected=Display state=UNAVAILABLE")
                  == 0,
          "ITEM-509: the installer record stays English in a Russian screen");
}

static void test_missing_translation_is_named(void)
{
    struct installer_ui_model model;
    size_t count;

    display_reset();
    installer_ui_foundation(&model, (enum appliance_language)7);
    CHECK(model.missing_translation,
          "ITEM-509: an unknown language latches the named refusal");
    count = installer_ui_lines(&model, (enum appliance_language)7, lines);
    CHECK(count == 1U && screen_has("INSTALLER TRANSLATION MISSING"),
          "ITEM-509: an unknown language renders the named refusal, not half "
          "a screen");
}

int main(void)
{
    display_reset();
    test_foundation_on_a_gop_machine();
    test_text_fallback_screen();
    test_degraded_display_is_not_available();
    test_navigation_and_selection();
    test_russian_foundation_screen();
    test_missing_translation_is_named();

    if (failures != 0) {
        fprintf(stderr, "Installer UI host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Installer UI host tests: ALL OK");
    return 0;
}
