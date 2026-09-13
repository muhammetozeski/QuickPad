#pragma once

#include <windows.h>

/*
 * Text is laid out with the advance width of every character in pixels, measured from the start of
 * its row, which is the start of the line unless word wrap splits the line into several rows.
 *
 * Widths are read from the font 256 UTF-16 units at a time, the first time a character of that page
 * is laid out, and kept in the metrics. Characters the font has no glyph for get cell widths: two
 * cells for wide East Asian characters and surrogate pairs, one for the rest. Combining marks and
 * zero-width characters take no width. A tab reaches the next tab stop.
 */

/* Fills widths[0..255] for the characters first..first + 255; entries it leaves alone keep cell widths. */
typedef void LayoutMeasure(void *context, wchar_t first, int *widths);

typedef struct LayoutMetrics {
    int *pages[256];
    int spare[256];
    int cellWidth;
    int tabWidth;
    LayoutMeasure *measure;
    void *context;
} LayoutMetrics;

#define LAYOUT_DEFAULT_TAB_CELLS 8

/* measure may be NULL, which lays every character out in cells. */
void LayoutMetricsInitialize(LayoutMetrics *metrics, int cellWidth, LayoutMeasure *measure, void *context);
void LayoutMetricsRelease(LayoutMetrics *metrics);
void LayoutSetTabCells(LayoutMetrics *metrics, int cells);

/* Reads a page of widths; call through LayoutCharWidth. */
int *LayoutLoadPage(LayoutMetrics *metrics, unsigned page);

/* Pixels taken by ch when it starts x pixels after the start of its row. */
static inline int LayoutCharWidth(LayoutMetrics *metrics, wchar_t ch, long long x)
{
    if (ch == L'\t') {
        return metrics->tabWidth - (int)(x % metrics->tabWidth);
    }
    int *page = metrics->pages[ch >> 8];
    if (page == NULL) {
        page = LayoutLoadPage(metrics, ch >> 8);
    }
    return page[ch & 0xFF];
}

/* Position reached after text[from, to) when text[from] starts x pixels after the start of its row. */
long long LayoutAdvance(LayoutMetrics *metrics, const wchar_t *text, size_t from, size_t to, long long x);

/* Offset in [from, to] nearest to x pixels from the start of the row that begins at text[from]. */
size_t LayoutOffsetAtX(LayoutMetrics *metrics, const wchar_t *text, size_t from, size_t to, long long x);

/*
 * Splits a line into rows at most wrapWidth pixels wide, breaking after spaces and tabs where
 * possible. Spaces may run past the edge. rows receives the start offset of each row and must have
 * room for length + 1 entries. wrapWidth 0 keeps the line in one row.
 */
size_t LayoutWrapLine(LayoutMetrics *metrics, const wchar_t *text, size_t length, long long wrapWidth, size_t *rows);
