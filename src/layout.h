#pragma once

#include <windows.h>

/*
 * Text is drawn on a grid of equal columns. Columns are counted from the start of a row,
 * which is the start of the line unless word wrap splits the line into several rows.
 */
#define LAYOUT_TAB_SIZE 8

/*
 * Columns taken by ch when it starts at column: a tab reaches the next tab stop, wide East Asian
 * characters and surrogate pairs take two, combining marks and zero-width characters none.
 */
size_t LayoutCharColumns(wchar_t ch, size_t column);

/* Column reached after text[from, to) when text[from] starts at column. */
size_t LayoutAdvance(const wchar_t *text, size_t from, size_t to, size_t column);

/*
 * Offset in [from, to] nearest to x pixels from the start of the row that begins at text[from],
 * with every column charWidth pixels wide.
 */
size_t LayoutOffsetAtX(const wchar_t *text, size_t from, size_t to, long long x, int charWidth);

/*
 * Splits a line into rows of at most wrapColumns columns, breaking after spaces and tabs where
 * possible. Spaces may run past the edge. rows receives the start offset of each row and must
 * have room for length + 1 entries. wrapColumns 0 keeps the line in one row.
 */
size_t LayoutWrapLine(const wchar_t *text, size_t length, size_t wrapColumns, size_t *rows);
