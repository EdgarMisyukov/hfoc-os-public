/*
 * ITEM-444: the screen draws lowercase letters as lowercase.
 *
 * The font carried 26 uppercase glyphs, and glyph_rows() folded every 'a'..'z'
 * into the matching capital before it looked one up.  The 2026-09-18 hardware
 * run showed exactly what that produces: the input path already delivered
 * lower case (`help` executed, `1234567890` typed as digits), and every
 * character that reached the framebuffer - boot records and echoed input alike
 * - was painted in upper case.
 *
 * The proof drives the real renderer, not a copy of the table: framebuffer.c is
 * linked into this binary, framebuffer_init() points it at a host surface, and
 * framebuffer_char()/framebuffer_text() paint into it exactly as they do on the
 * machine.  A glyph is then read back as the 16 rows of 8 bits the renderer
 * painted, and the cases below require the shapes the hardware run asked for:
 * lower case is painted, it differs from upper case everywhere, it is
 * internally distinct, and a string keeps its case on the way to the surface.
 */
#include "framebuffer.h"
#include "appliance_language.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SURFACE_WIDTH 128U
#define SURFACE_HEIGHT 32U

static uint32_t surface[SURFACE_WIDTH * SURFACE_HEIGHT];

static struct limine_framebuffer framebuffer = {
    .address = surface,
    .width = SURFACE_WIDTH,
    .height = SURFACE_HEIGHT,
    .pitch = SURFACE_WIDTH * 4U,
    .bpp = 32U,
};

static int failures;
static unsigned int checks;

#define CHECK(condition, message) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } \
} while (0)

static void clear_surface(void)
{
    memset(surface, 0, sizeof(surface));
}

/*
 * Paint one character at the surface origin and read the cell back as 7 rows of
 * 5 bits (bit 7 is the leftmost column).  `painted` reports whether the cell is
 * non-empty, and `spill` reports a pixel outside the 8x16 cell the console's
 * 6-pixel advance relies on.
 */
static void render_cell(char value, uint8_t rows[16], bool *painted, bool *spill)
{
    unsigned int row;
    unsigned int column;

    clear_surface();
    framebuffer_char(0U, 0U, value, 0xFFFFFFFFU, 1U);
    *painted = false;
    *spill = false;
    for (row = 0U; row < 16U; ++row) {
        uint8_t bits = 0U;
        for (column = 0U; column < 8U; ++column) {
            if (surface[row * SURFACE_WIDTH + column] == 0xFFFFFFFFU) {
                bits = (uint8_t)(bits | (1U << (7U - column)));
            }
        }
        rows[row] = bits;
        if (bits != 0U) {
            *painted = true;
        }
    }
    for (column = 8U; column < 16U; ++column) {
        for (row = 0U; row < 16U; ++row) {
            if (surface[row * SURFACE_WIDTH + column] != 0U) {
                *spill = true;
            }
        }
    }
    for (column = 0U; column < SURFACE_WIDTH; ++column) {
        if (surface[16U * SURFACE_WIDTH + column] != 0U) {
            *spill = true;
        }
    }
}

static bool same_cell(const uint8_t left[16], const uint8_t right[16])
{
    return memcmp(left, right, 16U) == 0;
}

/*
 * ITEM-509 increment 3: the same read-back for a code point, through the paint
 * path the console uses.
 */
static void render_codepoint_cell(uint32_t codepoint, uint8_t rows[16],
                                  bool *painted, bool *spill)
{
    unsigned int row;
    unsigned int column;

    clear_surface();
    framebuffer_char_codepoint(0U, 0U, codepoint, 0xFFFFFFFFU, 1U);
    *painted = false;
    *spill = false;
    for (row = 0U; row < 16U; ++row) {
        uint8_t bits = 0U;
        for (column = 0U; column < 8U; ++column) {
            if (surface[row * SURFACE_WIDTH + column] == 0xFFFFFFFFU) {
                bits = (uint8_t)(bits | (1U << (7U - column)));
            }
        }
        rows[row] = bits;
        if (bits != 0U) {
            *painted = true;
        }
    }
    for (column = 8U; column < 16U; ++column) {
        for (row = 0U; row < 16U; ++row) {
            if (surface[row * SURFACE_WIDTH + column] != 0U) {
                *spill = true;
            }
        }
    }
    for (column = 0U; column < SURFACE_WIDTH; ++column) {
        if (surface[16U * SURFACE_WIDTH + column] != 0U) {
            *spill = true;
        }
    }
}

static uint64_t surface_fingerprint(void)
{
    uint64_t hash = 1469598103934665603ULL;
    size_t index;

    for (index = 0U; index < sizeof(surface) / sizeof(surface[0]); ++index) {
        hash ^= surface[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t render_text_fingerprint(const char *text)
{
    clear_surface();
    framebuffer_text(0U, 0U, text, 0xFFFFFFFFU, 1U);
    return surface_fingerprint();
}

/*
 * ITEM-509 increment 17: the text path decodes UTF-8.
 *
 * The installer screens and the console's framebuffer backend hand
 * framebuffer_text() the catalog's UTF-8 text.  One code point must be one
 * cell - not three Latin glyphs for a two-byte letter - the ASCII path must
 * paint exactly what framebuffer_char() paints, and a code point the font does
 * not carry must draw the visible marker and be counted.
 */
static void read_text_cell(unsigned int cell_x, uint8_t rows[16])
{
    unsigned int row;
    unsigned int column;

    for (row = 0U; row < 16U; ++row) {
        uint8_t bits = 0U;

        for (column = 0U; column < 8U; ++column) {
            if (surface[row * SURFACE_WIDTH + cell_x + column] == 0xFFFFFFFFU) {
                bits = (uint8_t)(bits | (1U << (7U - column)));
            }
        }
        rows[row] = bits;
    }
}

static void test_utf8_text_path(void)
{
    uint8_t expected[16];
    uint8_t rows[16];
    uint64_t missing;

    CHECK(framebuffer_glyph_rows(0x0410U, expected) && expected[8] != 0U,
          "the Cyrillic table carries the letter the text path must draw");
    clear_surface();
    framebuffer_text(0U, 0U, "А", 0xFFFFFFFFU, 1U);
    read_text_cell(0U, rows);
    CHECK(same_cell(rows, expected),
          "a two-byte Russian letter is one cell with its own glyph");

    CHECK(framebuffer_glyph_rows((uint32_t)'A', expected) && expected[8] != 0U,
          "the Latin table carries the letter the ASCII path must draw");
    clear_surface();
    framebuffer_text(0U, 0U, "A", 0xFFFFFFFFU, 1U);
    read_text_cell(0U, rows);
    CHECK(same_cell(rows, expected),
          "the ASCII path keeps painting the Latin cell it always painted");

    clear_surface();
    framebuffer_text(0U, 0U, "АБ", 0xFFFFFFFFU, 1U);
    framebuffer_glyph_rows(0x0411U, expected);
    read_text_cell(8U, rows);
    CHECK(same_cell(rows, expected),
          "the cell advance counts code points, not bytes");

    missing = framebuffer_glyph_missing_count();
    clear_surface();
    framebuffer_text(0U, 0U, "\xE4\xB8\x80", 0xFFFFFFFFU, 1U);
    read_text_cell(0U, rows);
    CHECK(framebuffer_glyph_missing_count() == missing + 1U &&
              framebuffer_glyph_missing_last() == 0x4E00U && rows[3] != 0U,
          "a code point with no glyph draws the visible marker and is counted");

    missing = framebuffer_glyph_missing_count();
    framebuffer_glyph_rows((uint32_t)'B', expected);
    clear_surface();
    framebuffer_text(0U, 0U, "A\xD0" "B", 0xFFFFFFFFU, 1U);
    read_text_cell(16U, rows);
    CHECK(same_cell(rows, expected) &&
              framebuffer_glyph_missing_count() == missing + 1U,
          "a truncated sequence costs one cell and the line continues");
}

int main(void)
{
    uint8_t lowercase[26][16];
    uint8_t uppercase[26][16];
    bool lowercase_painted[26];
    bool uppercase_painted[26];
    bool spill = false;
    bool painted = false;
    unsigned int index;
    unsigned int other;
    char message[96];
    uint8_t glyph[16];

    CHECK(framebuffer_init(&framebuffer), "the host surface is accepted");
    CHECK(framebuffer_ready(), "the renderer is ready");

    for (index = 0U; index < 26U; ++index) {
        render_cell((char)('a' + index), lowercase[index], &lowercase_painted[index], &spill);
        CHECK(!spill, "a lowercase glyph stays inside its 8x16 cell");
        render_cell((char)('A' + index), uppercase[index], &uppercase_painted[index], &spill);
        CHECK(!spill, "an uppercase glyph stays inside its 8x16 cell");
        if (lowercase_painted[index] && uppercase_painted[index]) {
            snprintf(message, sizeof(message),
                "lower case '%c' differs from upper case '%c'",
                (char)('a' + index), (char)('A' + index));
            CHECK(!same_cell(lowercase[index], uppercase[index]), message);
        }
    }

    /* Every letter exists in both cases, and they are not the same picture. */
    for (index = 0U; index < 26U; ++index) {
        snprintf(message, sizeof(message), "lower case '%c' is painted",
            (char)('a' + index));
        CHECK(lowercase_painted[index], message);
        snprintf(message, sizeof(message), "upper case '%c' is painted",
            (char)('A' + index));
        CHECK(uppercase_painted[index], message);
    }

    /* A table with a duplicated row is a defect the case pair cannot see. */
    for (index = 0U; index < 26U; ++index) {
        for (other = index + 1U; other < 26U; ++other) {
            snprintf(message, sizeof(message),
                "lower case '%c' differs from lower case '%c'",
                (char)('a' + index), (char)('a' + other));
            CHECK(!same_cell(lowercase[index], lowercase[other]), message);
            snprintf(message, sizeof(message),
                "lower case '%c' differs from upper case '%c'",
                (char)('a' + index), (char)('A' + other));
            CHECK(!same_cell(lowercase[index], uppercase[other]), message);
        }
    }

    /* The rest of the font is untouched: digits and punctuation still paint. */
    for (index = 0U; index < 10U; ++index) {
        render_cell((char)('0' + index), glyph, &painted, &spill);
        snprintf(message, sizeof(message), "digit '%c' is painted", (char)('0' + index));
        CHECK(painted && !spill, message);
    }
    render_cell('-', glyph, &painted, &spill);
    CHECK(painted && !spill, "the punctuation of a record is still painted");

    /*
     * The surface is what the machine shows: a lowercase string and the same
     * string in upper case may not come out as the same picture, and the
     * lowercase picture has to differ from the upper case one at the very first
     * cell as well.
     */
    CHECK(render_text_fingerprint("hfoc os 123") !=
        render_text_fingerprint("HFOC OS 123"),
        "a string keeps its case on the way to the framebuffer");
    CHECK(render_text_fingerprint("help") != render_text_fingerprint("HELP"),
        "the echoed command keeps its case");
    CHECK(render_text_fingerprint("a") != render_text_fingerprint("A"),
        "the first cell of a string carries its case");
    CHECK(render_text_fingerprint("hfoc os 123") ==
        render_text_fingerprint("hfoc os 123"),
        "the same string renders the same picture twice");

    /*
     * ITEM-509 increment 3: the coverage gate.
     *
     * The font's contract with the catalog is total: every code point of every
     * Russian value the catalog carries has a glyph, so Russian text cannot
     * vanish into the old `default: break;`.  The inventory is the catalog's
     * own (`appliance_language_ru_codepoints()`, the same call the console's
     * language work uses), not a list copied here: a Russian string added to
     * the catalog without its glyph fails this proof by name.  Control code
     * points are the console's - a newline is a wrap, not a cell - and the
     * space is a glyph: the empty cell a space is drawn as.
     */
    {
        uint32_t catalog[512];
        size_t catalog_count =
            appliance_language_ru_codepoints(catalog,
                sizeof(catalog) / sizeof(catalog[0]));
        size_t catalog_index;

        CHECK(catalog_count != (size_t)-1,
              "the catalog's Russian code points fit the proof's inventory");
        CHECK(catalog_count != 0U,
              "the catalog carries Russian text to cover");
        for (catalog_index = 0U; catalog_index < catalog_count;
             ++catalog_index) {
            const uint32_t codepoint = catalog[catalog_index];

            if (codepoint < 0x20U || codepoint == 0x7FU) {
                continue; /* the console consumes control code points */
            }
            snprintf(message, sizeof(message),
                "the catalog's U+%04X has a glyph", codepoint);
            CHECK(framebuffer_glyph_rows(codepoint, glyph), message);
            render_codepoint_cell(codepoint, glyph, &painted, &spill);
            snprintf(message, sizeof(message),
                "the cell of U+%04X is painted", codepoint);
            CHECK((painted || codepoint == 0x20U) && !spill, message);
        }
    }

    /*
     * The two cases are different pictures, and Ё/ё are not Е/е: a table that
     * quietly folded one into the other would pass the coverage gate above.
     */
    {
        uint32_t index32;

        for (index32 = 0U; index32 < 32U; ++index32) {
            uint8_t upper[16];
            uint8_t lower[16];
            bool upper_painted = false;
            bool lower_painted = false;

            render_codepoint_cell(0x0410U + index32, upper, &upper_painted,
                                  &spill);
            render_codepoint_cell(0x0430U + index32, lower, &lower_painted,
                                  &spill);
            CHECK(upper_painted && lower_painted && !spill,
                  "a Cyrillic letter is painted in both cases");
            snprintf(message, sizeof(message),
                "the Cyrillic U+%04X differs from U+%04X",
                0x0410U + index32, 0x0430U + index32);
            CHECK(!same_cell(upper, lower), message);
        }
        {
            uint8_t yo_upper[16];
            uint8_t ye_upper[16];
            uint8_t yo_lower[16];
            uint8_t ye_lower[16];
            bool painted_flag = false;

            render_codepoint_cell(0x0401U, yo_upper, &painted_flag, &spill);
            render_codepoint_cell(0x0415U, ye_upper, &painted_flag, &spill);
            CHECK(!same_cell(yo_upper, ye_upper),
                  "Ё is not drawn as Е");
            render_codepoint_cell(0x0451U, yo_lower, &painted_flag, &spill);
            render_codepoint_cell(0x0435U, ye_lower, &painted_flag, &spill);
            CHECK(!same_cell(yo_lower, ye_lower),
                  "ё is not drawn as е");
        }
    }

    /*
     * A code point the font does not carry is not silence: the cell is a
     * visible mark, the counter moves and the last one is remembered.
     */
    {
        const uint64_t missing_before = framebuffer_glyph_missing_count();
        uint8_t empty[16];
        bool present;

        memset(empty, 0, sizeof(empty));
        present = framebuffer_glyph_rows(0x4E00U, empty);
        CHECK(!present, "U+4E00 is a code point the font does not carry");
        render_codepoint_cell(0x4E00U, glyph, &painted, &spill);
        CHECK(painted && !spill,
              "a code point with no glyph still paints a visible cell");
        CHECK(framebuffer_glyph_missing_count() == missing_before + 1U,
              "the missing glyph is counted");
        CHECK(framebuffer_glyph_missing_last() == 0x4E00U,
              "the last missing glyph is named");
    }

    /*
     * The ASCII predicate and the painted cell agree for every printable byte:
     * `framebuffer_glyph_rows()` is what the console asks before it decides to
     * publish a record, and a predicate that called a drawn letter missing (or
     * the other way round) would lie to the operator.  A byte the font does not
     * carry is not silent either - it is the same marker the Russian path uses
     * - while the space stays the empty cell it has always been.
     */
    {
        uint8_t marker[16];
        bool marker_painted = false;
        uint32_t ascii;

        render_codepoint_cell(0x4E00U, marker, &marker_painted, &spill);
        CHECK(marker_painted,
              "the missing-glyph marker is a painted cell");
        for (ascii = 0x20U; ascii < 0x7FU; ++ascii) {
            uint8_t cell[16];
            bool cell_painted = false;
            const bool present = framebuffer_glyph_rows(ascii, cell);

            render_codepoint_cell(ascii, cell, &cell_painted, &spill);
            snprintf(message, sizeof(message),
                "the ASCII predicate matches the cell of 0x%02X", ascii);
            if (ascii == 0x20U) {
                CHECK(present && !cell_painted, message);
            } else if (present) {
                CHECK(cell_painted && !same_cell(cell, marker), message);
            } else {
                CHECK(cell_painted && same_cell(cell, marker), message);
            }
        }
    }

    test_utf8_text_path();

    if (failures != 0) {
        fprintf(stderr, "Framebuffer glyph host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("Framebuffer glyph host test: OK (%u checks)\n", checks);
    return 0;
}
