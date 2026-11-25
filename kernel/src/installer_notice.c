#include "installer_notice.h"

#include "framebuffer.h"
#include "platform_reset.h"

/*
 * ITEM-417: the banner and the countdown an operator reads on the machine
 * itself.
 *
 * Colours are the ones the appliance console uses for its own frame, so the
 * screen a run ends on does not look like a different product's.
 */
#define INSTALLER_NOTICE_BACKGROUND UINT32_C(0x00101820)
#define INSTALLER_NOTICE_BAND UINT32_C(0x00203040)
#define INSTALLER_NOTICE_TITLE UINT32_C(0x00F0F4F8)
#define INSTALLER_NOTICE_TEXT UINT32_C(0x00C0D0E0)
#define INSTALLER_NOTICE_COUNT UINT32_C(0x00FFD070)
#define INSTALLER_NOTICE_TITLE_SCALE 4U
#define INSTALLER_NOTICE_COUNT_SCALE 8U
#define INSTALLER_NOTICE_TEXT_SCALE 2U
/* The countdown is written once per second, so each step is one delay. */
#define INSTALLER_NOTICE_TICK_MS 1000U

static void installer_notice_count(uint32_t seconds) {
    const uint64_t width = framebuffer_width();
    const uint64_t height = framebuffer_height();
    const uint64_t scale = INSTALLER_NOTICE_COUNT_SCALE;
    /* A number is centred by its widest case: two digits at this scale. */
    const uint64_t glyph_width = FRAMEBUFFER_FONT_CELL_WIDTH * scale;
    const uint64_t text_width = 2U * glyph_width;
    const uint64_t x = width > text_width ? (width - text_width) / 2U : 0U;
    const uint64_t y = height > 16U * scale ? height / 2U : 0U;

    /* The band the number lives in is repainted, so a shorter number never
     * leaves a digit of the previous one behind. */
    framebuffer_rect(x, y, text_width, 8U * scale,
                     INSTALLER_NOTICE_BACKGROUND);
    if (seconds >= 10U) {
        framebuffer_u64(x, y, seconds, INSTALLER_NOTICE_COUNT, scale);
    } else {
        framebuffer_u64(x + glyph_width, y, seconds,
                        INSTALLER_NOTICE_COUNT, scale);
    }
    /* ITEM-486: the countdown is drawn into the RAM shadow; flush it before the
       second-long delay so the operator reads the number that is current. */
    framebuffer_sync();
}

/*
 * ITEM-509 increment 19: the two sentences the banner prints live in the one
 * catalog; a language the catalog cannot answer returns NULL and the banner is
 * refused instead of drawn half-translated.
 */
const char *installer_notice_title(enum appliance_language language) {
    return appliance_language_string(language, "installer.notice.title");
}

const char *installer_notice_footer(enum appliance_language language) {
    return appliance_language_string(language, "installer.notice.remove_medium");
}

bool installer_notice_banner(enum appliance_language language,
                             uint32_t seconds) {
    const uint64_t width = framebuffer_width();
    const uint64_t height = framebuffer_height();
    const char *title = installer_notice_title(language);
    const char *footer = installer_notice_footer(language);
    uint64_t title_x;
    uint64_t title_y;
    uint64_t footer_y;
    uint32_t remaining;

    if (!framebuffer_ready() || title == NULL || footer == NULL ||
        width == 0U || height == 0U) {
        return false;
    }
    framebuffer_clear(INSTALLER_NOTICE_BACKGROUND);
    framebuffer_rect(0U, 0U, width, 12U * INSTALLER_NOTICE_TITLE_SCALE,
                     INSTALLER_NOTICE_BAND);
    title_x = 4U * INSTALLER_NOTICE_TITLE_SCALE;
    title_y = 3U * INSTALLER_NOTICE_TITLE_SCALE;
    framebuffer_text(title_x, title_y, title, INSTALLER_NOTICE_TITLE,
                     INSTALLER_NOTICE_TITLE_SCALE);
    footer_y = height > 12U * INSTALLER_NOTICE_TEXT_SCALE
                   ? height - 10U * INSTALLER_NOTICE_TEXT_SCALE
                   : 0U;
    framebuffer_text(4U * INSTALLER_NOTICE_TEXT_SCALE, footer_y, footer,
                     INSTALLER_NOTICE_TEXT, INSTALLER_NOTICE_TEXT_SCALE);
    /* ITEM-486: the banner itself is a visible write, flush it before the
       countdown's own once-a-second flushes take over. */
    framebuffer_sync();

    /*
     * One delay per second, each showing the seconds that are left: the message
     * is a countdown on the screen, not a fixed picture that happens to be on
     * it while the machine waits.
     */
    remaining = seconds;
    while (remaining > 0U) {
        installer_notice_count(remaining);
        platform_reset_delay_ms(INSTALLER_NOTICE_TICK_MS);
        --remaining;
    }
    return true;
}
