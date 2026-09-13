/* Unit tests for src/search.c: find in both directions, whole words, case and replace all. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "search.h"
#include "quickpad.h"

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

static BOOL Find(const wchar_t *text, size_t from, const wchar_t *pattern, BOOL matchCase, BOOL wholeWord, BOOL down,
    size_t *found)
{
    SearchOptions options = { matchCase, wholeWord };
    return SearchFind(text, wcslen(text), from, pattern, wcslen(pattern), options, down, found);
}

static void ExpectReplaceAll(const char *name, const wchar_t *text, const wchar_t *pattern, const wchar_t *with,
    BOOL matchCase, BOOL wholeWord, size_t expectedCount, const wchar_t *expected)
{
    SearchOptions options = { matchCase, wholeWord };
    wchar_t *replacement = NULL;
    size_t replacementLength = 0;
    size_t start = 0;
    size_t end = 0;
    size_t length = wcslen(text);
    size_t count = SearchReplaceAll(text, length, pattern, wcslen(pattern), with, wcslen(with), options,
        &replacement, &replacementLength, &start, &end);
    CHECK(count == expectedCount, name);
    if (count == 0) {
        CHECK(replacement == NULL, name);
        return;
    }

    wchar_t result[256];
    memcpy(result, text, start * sizeof(wchar_t));
    memcpy(result + start, replacement, replacementLength * sizeof(wchar_t));
    memcpy(result + start + replacementLength, text + end, (length - end) * sizeof(wchar_t));
    result[start + replacementLength + length - end] = 0;
    CHECK(wcscmp(result, expected) == 0, name);
    MemFree(replacement);
}

int wmain(void)
{
    size_t found = 0;
    const wchar_t *text = L"one two One three one";

    CHECK(Find(text, 0, L"one", TRUE, FALSE, TRUE, &found) && found == 0, "first match");
    CHECK(Find(text, 1, L"one", TRUE, FALSE, TRUE, &found) && found == 18, "match case skips One");
    CHECK(Find(text, 1, L"one", FALSE, FALSE, TRUE, &found) && found == 8, "ignore case finds One");
    CHECK(Find(text, 19, L"one", TRUE, FALSE, TRUE, &found) && found == 0, "down wraps to the start");
    CHECK(Find(text, 18, L"one", FALSE, FALSE, FALSE, &found) && found == 8, "up finds the previous match");
    CHECK(Find(text, 2, L"one", TRUE, FALSE, FALSE, &found) && found == 18, "up wraps to the end");
    CHECK(!Find(text, 0, L"four", FALSE, FALSE, TRUE, &found), "missing pattern");
    CHECK(!Find(text, 0, L"", FALSE, FALSE, TRUE, &found), "empty pattern");

    const wchar_t *words = L"cat concat cat_ cats cat";
    CHECK(Find(words, 1, L"cat", TRUE, TRUE, TRUE, &found) && found == 21, "whole word skips joined matches");
    CHECK(Find(words, 21, L"cat", TRUE, TRUE, FALSE, &found) && found == 0, "whole word up");
    CHECK(Find(L"a-b", 0, L"-", TRUE, TRUE, TRUE, &found) && found == 1, "punctuation pattern is a whole word");
    CHECK(Find(L"\x011F\x00FC\x015F g\x00FC", 0, L"G\x00DC", FALSE, FALSE, TRUE, &found) && found == 4, "ignore case outside ascii");

    ExpectReplaceAll("replace all", L"a1a2a3", L"a", L"bb", TRUE, FALSE, 3, L"bb1bb2bb3");
    ExpectReplaceAll("replace with nothing", L"x--y--z", L"--", L"", TRUE, FALSE, 2, L"xyz");
    ExpectReplaceAll("replace whole words", L"cat concat cat", L"cat", L"dog", TRUE, TRUE, 2, L"dog concat dog");
    ExpectReplaceAll("replace ignoring case", L"Ab ab AB", L"ab", L"c", FALSE, FALSE, 3, L"c c c");
    ExpectReplaceAll("non-overlapping matches", L"aaaa", L"aa", L"b", TRUE, FALSE, 2, L"bb");
    ExpectReplaceAll("no match", L"abc", L"z", L"y", TRUE, FALSE, 0, L"abc");
    ExpectReplaceAll("match in the middle only", L"xxabyy", L"ab", L"line\nbreak", TRUE, FALSE, 1, L"xxline\nbreakyy");

    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
