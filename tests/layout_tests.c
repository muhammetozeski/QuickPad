/* Unit tests for src/layout.c: column widths, hit testing and word wrap. */
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

static void ExpectRows(const char *name, const wchar_t *text, size_t wrapColumns, const size_t *expected, size_t expectedCount)
{
    size_t rows[128];
    size_t count = LayoutWrapLine(text, wcslen(text), wrapColumns, rows);
    CHECK(count == expectedCount, name);
    CHECK(count == expectedCount && memcmp(rows, expected, count * sizeof(size_t)) == 0, name);
}

#define ROWS(...) ((const size_t[]){ __VA_ARGS__ }), sizeof((const size_t[]){ __VA_ARGS__ }) / sizeof(size_t)

int wmain(void)
{
    CHECK(LayoutCharColumns(L'a', 0) == 1, "ascii is one column");
    CHECK(LayoutCharColumns(L'\t', 0) == LAYOUT_TAB_SIZE, "tab at column 0");
    CHECK(LayoutCharColumns(L'\t', 3) == LAYOUT_TAB_SIZE - 3, "tab reaches the next stop");
    CHECK(LayoutCharColumns(L'\t', LAYOUT_TAB_SIZE) == LAYOUT_TAB_SIZE, "tab on a stop takes a full stop");
    CHECK(LayoutCharColumns(0x4E2D, 0) == 2, "cjk is wide");
    CHECK(LayoutCharColumns(0xD83D, 0) == 2 && LayoutCharColumns(0xDE00, 2) == 0, "surrogate pair takes two columns");
    CHECK(LayoutCharColumns(0x0301, 1) == 0, "combining mark takes no column");
    CHECK(LayoutCharColumns(0x011F, 0) == 1, "turkish letter is one column");

    CHECK(LayoutAdvance(L"ab\tc", 0, 4, 0) == LAYOUT_TAB_SIZE + 1, "advance over a tab");
    CHECK(LayoutAdvance(L"a\x4E2D" L"b", 0, 3, 0) == 4, "advance over a wide character");

    const wchar_t *hit = L"ab\tc";
    CHECK(LayoutOffsetAtX(hit, 0, 4, 0, 10) == 0, "left edge");
    CHECK(LayoutOffsetAtX(hit, 0, 4, 4, 10) == 0, "left half of the first cell");
    CHECK(LayoutOffsetAtX(hit, 0, 4, 6, 10) == 1, "right half of the first cell");
    CHECK(LayoutOffsetAtX(hit, 0, 4, 45, 10) == 2, "left half of a tab");
    CHECK(LayoutOffsetAtX(hit, 0, 4, 55, 10) == 3, "right half of a tab");
    CHECK(LayoutOffsetAtX(hit, 0, 4, 1000, 10) == 4, "past the end");
    CHECK(LayoutOffsetAtX(L"e\x0301" L"x", 0, 3, 6, 10) == 2, "combining mark stays with its base");

    ExpectRows("no wrap", L"hello world", 0, ROWS(0));
    ExpectRows("fits", L"hello", 5, ROWS(0));
    ExpectRows("break after space", L"hello world foo", 11, ROWS(0, 12));
    ExpectRows("spaces run past the edge", L"abc     def", 4, ROWS(0, 8));
    ExpectRows("long word is cut", L"abcdefghij", 4, ROWS(0, 4, 8));
    ExpectRows("tab is a break", L"a\tb", 8, ROWS(0, 2));
    ExpectRows("wide characters", L"\x4E2D\x4E2D\x4E2D", 4, ROWS(0, 2));
    ExpectRows("wide character wider than the row", L"\x4E2D\x4E2D", 1, ROWS(0, 1));
    ExpectRows("surrogate pair is not split", L"a\xD83D\xDE00", 2, ROWS(0, 1));
    ExpectRows("empty line", L"", 10, ROWS(0));
    ExpectRows("word after a cut word", L"abcdef gh", 4, ROWS(0, 4, 7));

    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
