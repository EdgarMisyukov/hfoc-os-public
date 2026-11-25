/*
 * ITEM-509 increment 19: the installer banner's own sentences, proven on the
 * host.
 *
 * The banner (ITEM-417) is the screen the machine ends an install on: title,
 * the "remove the medium" footer and the countdown between them.  Its two
 * sentences now live in the one catalog, so this proof pins the contract the
 * surfaces depend on:
 *
 *   * the English title and footer are byte for byte the literals the banner
 *     used to carry, so the QEMU proof that reads the painted banner and the
 *     `[PRODUCTION-INSTALL] notice screen=...` record cannot move;
 *   * the Russian sentences come from the catalog column and differ from the
 *     English ones - the font carries Cyrillic and the text path decodes it,
 *     so a Russian banner would be drawn;
 *   * a language the catalog cannot answer returns NULL for both, which the
 *     banner turns into its named refusal rather than half a screen silently
 *     in English.
 *
 * The module under proof also owns the painting, so this file provides the
 * framebuffer and reset symbols it references.  The banner itself is not driven
 * here: it counts real seconds down through `platform_reset_delay_ms`, which is
 * the QEMU proof's job, not a host proof's.
 */
#include "installer_notice.h"

#include <stdbool.h>
#include <stdint.h>
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

/* The painting surface the banner uses; this proof never paints. */
bool framebuffer_ready(void) { return false; }
void framebuffer_clear(uint32_t colour) { (void)colour; }
void framebuffer_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height,
                      uint32_t colour) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)colour;
}
void framebuffer_text(uint64_t x, uint64_t y, const char *text,
                      uint32_t colour, uint32_t scale) {
    (void)x;
    (void)y;
    (void)text;
    (void)colour;
    (void)scale;
}
void framebuffer_u64(uint64_t x, uint64_t y, uint64_t value, uint32_t colour,
                     uint32_t scale) {
    (void)x;
    (void)y;
    (void)value;
    (void)colour;
    (void)scale;
}
uint64_t framebuffer_width(void) { return 0U; }
uint64_t framebuffer_height(void) { return 0U; }
void framebuffer_sync(void) {}
void platform_reset_delay_ms(uint32_t milliseconds) { (void)milliseconds; }

static void test_the_english_banner_is_the_literal_it_always_was(void)
{
    const char *title = installer_notice_title(APPLIANCE_LANGUAGE_DEFAULT);
    const char *footer = installer_notice_footer(APPLIANCE_LANGUAGE_DEFAULT);

    CHECK(title != NULL && strcmp(title, "HFOC OS INSTALLED") == 0,
          "the English title is the literal the banner always printed");
    CHECK(footer != NULL &&
              strcmp(footer,
                     "REMOVE THE INSTALLATION MEDIUM WHILE THE MACHINE "
                     "RESTARTS") == 0,
          "the English footer is the literal the banner always printed");
    CHECK(installer_notice_title(APPLIANCE_LANGUAGE_EN) == title ||
              strcmp(installer_notice_title(APPLIANCE_LANGUAGE_EN), title) ==
                  0,
          "the default language and `en` name the same title");
    CHECK(installer_notice_footer(APPLIANCE_LANGUAGE_EN) != NULL &&
              strcmp(installer_notice_footer(APPLIANCE_LANGUAGE_EN), footer) ==
                  0,
          "the default language and `en` name the same footer");
}

static void test_the_russian_banner_comes_from_the_catalog(void)
{
    const char *title = installer_notice_title(APPLIANCE_LANGUAGE_RU);
    const char *footer = installer_notice_footer(APPLIANCE_LANGUAGE_RU);

    CHECK(title != NULL && strcmp(title, "HFOC OS УСТАНОВЛЕНА") == 0,
          "the Russian title comes from the catalog column");
    CHECK(footer != NULL &&
              strcmp(footer,
                     "ИЗВЛЕКИТЕ НОСИТЕЛЬ УСТАНОВКИ, ПОКА МАШИНА "
                     "ПЕРЕЗАГРУЖАЕТСЯ") == 0,
          "the Russian footer comes from the catalog column");
    CHECK(strcmp(title, installer_notice_title(APPLIANCE_LANGUAGE_DEFAULT)) !=
              0 &&
              strcmp(footer,
                     installer_notice_footer(APPLIANCE_LANGUAGE_DEFAULT)) != 0,
          "the Russian banner is a translation, not the English text again");
}

static void test_a_language_the_catalog_cannot_answer_is_named(void)
{
    CHECK(installer_notice_title((enum appliance_language)7) == NULL &&
              installer_notice_footer((enum appliance_language)7) == NULL,
          "an unknown language answers NULL instead of falling back to "
          "English");
    /*
     * No framebuffer in this proof, so the banner's own refusal is what the
     * caller records as `notice screen=NONE reason=BANNER_FAILED`.
     */
    CHECK(!installer_notice_banner(APPLIANCE_LANGUAGE_DEFAULT, 0U),
          "a banner with no framebuffer refuses instead of claiming a screen");
}

int main(void)
{
    test_the_english_banner_is_the_literal_it_always_was();
    test_the_russian_banner_comes_from_the_catalog();
    test_a_language_the_catalog_cannot_answer_is_named();

    if (failures != 0) {
        fprintf(stderr, "Installer notice host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Installer notice host tests: ALL OK");
    return 0;
}
