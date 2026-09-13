/* Unit tests for src/layout.c: character widths, hit testing and word wrap. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "layout.h"

static int failures;
static int checks;

#define CHECK(condition, name)                                                  \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            wprintf(L"FAIL %hs: %hs (line %d)\n", name, #condition, __LINE__);  \
        }                                                                       \
    } while (0)

#define CELL 10

static LayoutMetrics cells;
static LayoutMetrics proportional;

/* A proportional font for the tests: 'i' is 4 pixels, 'W' 16, digits and other letters keep cell widths. */
static void MeasureProportional(void *context, wchar_t first, int *widths)
{
    (void)context;
    if (first == 0) {
        widths[L'i'] = 4;
        widths[L'W'] = 16;
        widths[L' '] = 5;
    }
}

static void ExpectRows(const char *name, LayoutMetrics *metrics, const wchar_t *text, long long wrapWidth,
    const size_t *expected, size_t expectedCount)
{
    size_t rows[128];
    size_t count = LayoutWrapLine(metrics, text, wcslen(text), wrapWidth, rows);
    CHECK(count == expectedCount, name);
    CHECK(count == expectedCount && memcmp(rows, expected, count * sizeof(size_t)) == 0, name);
}

#define ROWS(...) ((const size_t[]){ __VA_ARGS__ }), sizeof((const size_t[]){ __VA_ARGS__ }) / sizeof(size_t)

int wmain(void)
{
    LayoutMetricsInitialize(&cells, CELL, NULL, NULL);
    LayoutMetricsInitialize(&proportional, CELL, MeasureProportional, NULL);

    CHECK(LayoutCharWidth(&cells, L'a', 0) == CELL, "ascii is one cell");
    CHECK(LayoutCharWidth(&cells, L'\t', 0) == 8 * CELL, "tab at the row start");
    CHECK(LayoutCharWidth(&cells, L'\t', 3 * CELL) == 5 * CELL, "tab reaches the next stop");
    CHECK(LayoutCharWidth(&cells, L'\t', 8 * CELL) == 8 * CELL, "tab on a stop takes a full stop");
    CHECK(LayoutCharWidth(&cells, 0x4E2D, 0) == 2 * CELL, "cjk is wide");
    CHECK(LayoutCharWidth(&cells, 0xD83D, 0) == 2 * CELL && LayoutCharWidth(&cells, 0xDE00, 0) == 0, "surrogate pair takes two cells");
    CHECK(LayoutCharWidth(&cells, 0x0301, 0) == 0, "combining mark takes no width");
    CHECK(LayoutCharWidth(&cells, 0x011F, 0) == CELL, "turkish letter is one cell");

    LayoutSetTabCells(&cells, 4);
    CHECK(LayoutCharWidth(&cells, L'\t', CELL) == 3 * CELL, "tab size four");
    LayoutSetTabCells(&cells, LAYOUT_DEFAULT_TAB_CELLS);

    CHECK(LayoutAdvance(&cells, L"ab\tc", 0, 4, 0) == 9 * CELL, "advance over a tab");
    CHECK(LayoutAdvance(&cells, L"a\x4E2D" L"b", 0, 3, 0) == 4 * CELL, "advance over a wide character");
    CHECK(LayoutAdvance(&proportional, L"iWa", 0, 3, 0) == 4 + 16 + CELL, "advance with measured widths");
    CHECK(LayoutAdvance(&proportional, L"i\t", 0, 2, 0) == 8 * CELL, "tab after a narrow letter reaches the stop");

    const wchar_t *hit = L"ab\tc";
    CHECK(LayoutOffsetAtX(&cells, hit, 0, 4, 0) == 0, "left edge");
    CHECK(LayoutOffsetAtX(&cells, hit, 0, 4, 4) == 0, "left half of the first cell");
    CHECK(LayoutOffsetAtX(&cells, hit, 0, 4, 6) == 1, "right half of the first cell");
    CHECK(LayoutOffsetAtX(&cells, hit, 0, 4, 45) == 2, "left half of a tab");
    CHECK(LayoutOffsetAtX(&cells, hit, 0, 4, 55) == 3, "right half of a tab");
    CHECK(LayoutOffsetAtX(&cells, hit, 0, 4, 1000) == 4, "past the end");
    CHECK(LayoutOffsetAtX(&cells, L"e\x0301" L"x", 0, 3, 6) == 2, "combining mark stays with its base");
    CHECK(LayoutOffsetAtX(&proportional, L"iWi", 0, 3, 11) == 1 && LayoutOffsetAtX(&proportional, L"iWi", 0, 3, 13) == 2,
        "hit testing with measured widths");

    ExpectRows("no wrap", &cells, L"hello world", 0, ROWS(0));
    ExpectRows("fits", &cells, L"hello", 5 * CELL, ROWS(0));
    ExpectRows("break after space", &cells, L"hello world foo", 11 * CELL, ROWS(0, 12));
    ExpectRows("spaces run past the edge", &cells, L"abc     def", 4 * CELL, ROWS(0, 8));
    ExpectRows("long word is cut", &cells, L"abcdefghij", 4 * CELL, ROWS(0, 4, 8));
    ExpectRows("tab is a break", &cells, L"a\tb", 8 * CELL, ROWS(0, 2));
    ExpectRows("wide characters", &cells, L"\x4E2D\x4E2D\x4E2D", 4 * CELL, ROWS(0, 2));
    ExpectRows("wide character wider than the row", &cells, L"\x4E2D\x4E2D", CELL, ROWS(0, 1));
    ExpectRows("surrogate pair is not split", &cells, L"a\xD83D\xDE00", 2 * CELL, ROWS(0, 1));
    ExpectRows("empty line", &cells, L"", 10 * CELL, ROWS(0));
    ExpectRows("word after a cut word", &cells, L"abcdef gh", 4 * CELL, ROWS(0, 4, 7));
    ExpectRows("narrow letters fit more", &proportional, L"iiiiiiiiii WW", 50, ROWS(0, 11));

    LayoutMetricsRelease(&proportional);
    LayoutMetricsRelease(&cells);
    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
