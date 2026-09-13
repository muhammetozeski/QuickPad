#include "layout.h"
#include "quickpad.h"

static BOOL IsZeroWidth(wchar_t ch)
{
    return (ch >= 0x0300 && ch <= 0x036F) || (ch >= 0x1AB0 && ch <= 0x1AFF) || (ch >= 0x1DC0 && ch <= 0x1DFF)
        || (ch >= 0x200B && ch <= 0x200F) || (ch >= 0x20D0 && ch <= 0x20FF) || (ch >= 0xFE20 && ch <= 0xFE2F)
        || ch == 0xFEFF;
}

static BOOL IsWide(wchar_t ch)
{
    return (ch >= 0x1100 && ch <= 0x115F) || (ch >= 0x2E80 && ch <= 0x303E) || (ch >= 0x3041 && ch <= 0x33FF)
        || (ch >= 0x3400 && ch <= 0x4DBF) || (ch >= 0x4E00 && ch <= 0x9FFF) || (ch >= 0xA000 && ch <= 0xA4CF)
        || (ch >= 0xAC00 && ch <= 0xD7A3) || (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0xFE30 && ch <= 0xFE4F)
        || (ch >= 0xFF00 && ch <= 0xFF60) || (ch >= 0xFFE0 && ch <= 0xFFE6);
}

void LayoutMetricsInitialize(LayoutMetrics *metrics, int cellWidth, LayoutMeasure *measure, void *context)
{
    for (size_t i = 0; i < ARRAYSIZE(metrics->pages); ++i) {
        metrics->pages[i] = NULL;
    }
    metrics->cellWidth = cellWidth > 0 ? cellWidth : 1;
    metrics->tabWidth = metrics->cellWidth * LAYOUT_DEFAULT_TAB_CELLS;
    metrics->measure = measure;
    metrics->context = context;
}

void LayoutMetricsRelease(LayoutMetrics *metrics)
{
    for (size_t i = 0; i < ARRAYSIZE(metrics->pages); ++i) {
        MemFree(metrics->pages[i]);
        metrics->pages[i] = NULL;
    }
}

void LayoutSetTabCells(LayoutMetrics *metrics, int cells)
{
    metrics->tabWidth = metrics->cellWidth * (cells > 0 ? cells : 1);
}

int *LayoutLoadPage(LayoutMetrics *metrics, unsigned page)
{
    int *widths = MemAlloc(256 * sizeof(int));
    BOOL owned = widths != NULL;
    if (!owned) {
        widths = metrics->spare;
    }

    wchar_t first = (wchar_t)(page << 8);
    int cell = metrics->cellWidth;
    for (int i = 0; i < 256; ++i) {
        widths[i] = IsWide((wchar_t)(first + i)) ? 2 * cell : cell;
    }
    if (metrics->measure != NULL) {
        metrics->measure(metrics->context, first, widths);
    }
    for (int i = 0; i < 256; ++i) {
        wchar_t ch = (wchar_t)(first + i);
        if (IsZeroWidth(ch) || IS_LOW_SURROGATE(ch)) {
            widths[i] = 0;
        } else if (IS_HIGH_SURROGATE(ch)) {
            widths[i] = 2 * cell;
        } else if (widths[i] < 0) {
            widths[i] = cell;
        }
    }

    if (owned) {
        metrics->pages[page] = widths;
    }
    return widths;
}

long long LayoutAdvance(LayoutMetrics *metrics, const wchar_t *text, size_t from, size_t to, long long x)
{
    for (size_t i = from; i < to; ++i) {
        x += LayoutCharWidth(metrics, text[i], x);
    }
    return x;
}

size_t LayoutOffsetAtX(LayoutMetrics *metrics, const wchar_t *text, size_t from, size_t to, long long x)
{
    long long left = 0;
    for (size_t i = from; i < to; ++i) {
        int width = LayoutCharWidth(metrics, text[i], left);
        if (width == 0) {
            continue;
        }
        if (x * 2 < left * 2 + width) {
            return i;
        }
        left += width;
    }
    return to;
}

size_t LayoutWrapLine(LayoutMetrics *metrics, const wchar_t *text, size_t length, long long wrapWidth, size_t *rows)
{
    size_t count = 0;
    rows[count++] = 0;
    if (wrapWidth <= 0) {
        return count;
    }

    size_t rowStart = 0;
    long long x = 0;
    size_t breakAfterSpace = 0;
    for (size_t i = 0; i < length; ++i) {
        wchar_t ch = text[i];
        BOOL space = ch == L' ' || ch == L'\t';
        int width = LayoutCharWidth(metrics, ch, x);
        if (!space && width > 0 && x + width > wrapWidth && i > rowStart) {
            rowStart = breakAfterSpace > rowStart ? breakAfterSpace : i;
            rows[count++] = rowStart;
            x = LayoutAdvance(metrics, text, rowStart, i, 0);
            breakAfterSpace = rowStart;
            width = LayoutCharWidth(metrics, ch, x);
        }
        x += width;
        if (space) {
            breakAfterSpace = i + 1;
        }
    }
    return count;
}
