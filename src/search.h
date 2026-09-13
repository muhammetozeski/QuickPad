#pragma once

#include <windows.h>

typedef struct SearchOptions {
    BOOL matchCase;
    BOOL wholeWord;
} SearchOptions;

/*
 * Finds pattern in text. Searching down starts at from; searching up takes the last match that
 * ends at or before from. When nothing is found that way the search wraps around the other end.
 */
BOOL SearchFind(const wchar_t *text, size_t length, size_t from, const wchar_t *pattern, size_t patternLength,
    SearchOptions options, BOOL down, size_t *found);

/*
 * Replaces every match. Returns the number of matches; when it is not zero, *replacement is a
 * MemAlloc block holding the new text for [*rangeStart, *rangeEnd) of the original text.
 */
size_t SearchReplaceAll(const wchar_t *text, size_t length, const wchar_t *pattern, size_t patternLength,
    const wchar_t *with, size_t withLength, SearchOptions options, wchar_t **replacement, size_t *replacementLength,
    size_t *rangeStart, size_t *rangeEnd);

/* TRUE when text[position, position + count) is not joined to letters, digits or underscores around it. */
BOOL SearchIsWholeWord(const wchar_t *text, size_t length, size_t position, size_t count);
