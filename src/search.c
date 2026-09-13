#include "search.h"
#include "quickpad.h"

/* FindStringOrdinal takes int lengths. */
#define SEARCH_MAX_LENGTH 0x7FFFFFFFu

static BOOL IsWordCharacter(wchar_t ch)
{
    return ch == L'_' || IsCharAlphaNumericW(ch);
}

BOOL SearchIsWholeWord(const wchar_t *text, size_t length, size_t position, size_t count)
{
    if (position > 0 && IsWordCharacter(text[position - 1]) && IsWordCharacter(text[position])) {
        return FALSE;
    }
    size_t end = position + count;
    return !(end < length && IsWordCharacter(text[end]) && IsWordCharacter(text[end - 1]));
}

/* First match lying entirely inside [start, end). */
static BOOL FindForward(const wchar_t *text, size_t length, size_t start, size_t end, const wchar_t *pattern,
    size_t patternLength, SearchOptions options, size_t *found)
{
    size_t position = start;
    while (position + patternLength <= end) {
        int index = FindStringOrdinal(FIND_FROMSTART, text + position, (int)(end - position), pattern,
            (int)patternLength, !options.matchCase);
        if (index < 0) {
            return FALSE;
        }
        size_t match = position + (size_t)index;
        if (!options.wholeWord || SearchIsWholeWord(text, length, match, patternLength)) {
            *found = match;
            return TRUE;
        }
        position = match + 1;
    }
    return FALSE;
}

/* Last match lying entirely inside [start, end). */
static BOOL FindBackward(const wchar_t *text, size_t length, size_t start, size_t end, const wchar_t *pattern,
    size_t patternLength, SearchOptions options, size_t *found)
{
    while (end >= start + patternLength) {
        int index = FindStringOrdinal(FIND_FROMEND, text + start, (int)(end - start), pattern, (int)patternLength,
            !options.matchCase);
        if (index < 0) {
            return FALSE;
        }
        size_t match = start + (size_t)index;
        if (!options.wholeWord || SearchIsWholeWord(text, length, match, patternLength)) {
            *found = match;
            return TRUE;
        }
        end = match + patternLength - 1;
    }
    return FALSE;
}

BOOL SearchFind(const wchar_t *text, size_t length, size_t from, const wchar_t *pattern, size_t patternLength,
    SearchOptions options, BOOL down, size_t *found)
{
    if (patternLength == 0 || patternLength > length || length > SEARCH_MAX_LENGTH) {
        return FALSE;
    }
    if (from > length) {
        from = length;
    }
    if (down) {
        return FindForward(text, length, from, length, pattern, patternLength, options, found)
            || FindForward(text, length, 0, length, pattern, patternLength, options, found);
    }
    return FindBackward(text, length, 0, from, pattern, patternLength, options, found)
        || FindBackward(text, length, 0, length, pattern, patternLength, options, found);
}

size_t SearchReplaceAll(const wchar_t *text, size_t length, const wchar_t *pattern, size_t patternLength,
    const wchar_t *with, size_t withLength, SearchOptions options, wchar_t **replacement, size_t *replacementLength,
    size_t *rangeStart, size_t *rangeEnd)
{
    *replacement = NULL;
    *replacementLength = 0;
    if (patternLength == 0 || patternLength > length || length > SEARCH_MAX_LENGTH) {
        return 0;
    }

    size_t count = 0;
    size_t first = 0;
    size_t last = 0;
    size_t match = 0;
    for (size_t position = 0; FindForward(text, length, position, length, pattern, patternLength, options, &match);
        position = match + patternLength) {
        if (count++ == 0) {
            first = match;
        }
        last = match;
    }
    if (count == 0) {
        return 0;
    }

    size_t end = last + patternLength;
    size_t newLength = end - first - count * patternLength + count * withLength;
    wchar_t *result = MemAlloc((newLength + 1) * sizeof(wchar_t));
    if (result == NULL) {
        return 0;
    }

    size_t out = 0;
    size_t position = first;
    while (position < end && FindForward(text, length, position, end, pattern, patternLength, options, &match)) {
        memcpy(result + out, text + position, (match - position) * sizeof(wchar_t));
        out += match - position;
        if (withLength > 0) {
            memcpy(result + out, with, withLength * sizeof(wchar_t));
            out += withLength;
        }
        position = match + patternLength;
    }

    *replacement = result;
    *replacementLength = out;
    *rangeStart = first;
    *rangeEnd = end;
    return count;
}
