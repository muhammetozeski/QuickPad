#include "layout.h"

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

size_t LayoutCharColumns(wchar_t ch, size_t column)
{
    if (ch < 0x0300) {
        return ch == L'\t' ? LAYOUT_TAB_SIZE - column % LAYOUT_TAB_SIZE : 1;
    }
    if (ch >= 0xDC00 && ch <= 0xDFFF) {
        return 0;
    }
    if (ch >= 0xD800 && ch <= 0xDBFF) {
        return 2;
    }
    if (IsZeroWidth(ch)) {
        return 0;
    }
    return IsWide(ch) ? 2 : 1;
}

size_t LayoutAdvance(const wchar_t *text, size_t from, size_t to, size_t column)
{
    for (size_t i = from; i < to; ++i) {
        column += LayoutCharColumns(text[i], column);
    }
    return column;
}

size_t LayoutOffsetAtX(const wchar_t *text, size_t from, size_t to, long long x, int charWidth)
{
    size_t column = 0;
    for (size_t i = from; i < to; ++i) {
        size_t width = LayoutCharColumns(text[i], column);
        if (width == 0) {
            continue;
        }
        long long left = (long long)column * charWidth;
        long long right = (long long)(column + width) * charWidth;
        if (x * 2 < left + right) {
            return i;
        }
        column += width;
    }
    return to;
}

size_t LayoutWrapLine(const wchar_t *text, size_t length, size_t wrapColumns, size_t *rows)
{
    size_t count = 0;
    rows[count++] = 0;
    if (wrapColumns == 0) {
        return count;
    }

    size_t rowStart = 0;
    size_t column = 0;
    size_t breakAfterSpace = 0;
    for (size_t i = 0; i < length; ++i) {
        wchar_t ch = text[i];
        BOOL space = ch == L' ' || ch == L'\t';
        size_t width = LayoutCharColumns(ch, column);
        if (!space && width > 0 && column + width > wrapColumns && i > rowStart) {
            rowStart = breakAfterSpace > rowStart ? breakAfterSpace : i;
            rows[count++] = rowStart;
            column = LayoutAdvance(text, rowStart, i, 0);
            breakAfterSpace = rowStart;
            width = LayoutCharColumns(ch, column);
        }
        column += width;
        if (space) {
            breakAfterSpace = i + 1;
        }
    }
    return count;
}
