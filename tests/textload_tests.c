/* Tests for src/textload.c: every input is decoded through a load and through TextDecode, and both must agree. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "blocks.h"
#include "document.h"
#include "quickpad.h"
#include "text.h"
#include "textload.h"

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

static void CountRelease(void *context)
{
    ++*(int *)context;
}

/* A source whose first part is handed over and whose rest is copied in when the load asks for it. */
typedef struct Source {
    unsigned char *buffer;
    const unsigned char *whole;
    int releases;
    int reads;
    BOOL fail;
} Source;

static BOOL ReadSource(void *context, size_t offset, size_t length)
{
    Source *source = context;
    ++source->reads;
    if (source->fail) {
        SetLastError(ERROR_READ_FAULT);
        return FALSE;
    }
    memcpy(source->buffer + offset, source->whole + offset, length);
    return TRUE;
}

static void ReleaseSource(void *context)
{
    ++((Source *)context)->releases;
}

/* Decodes data both ways and checks text, line index and format agree; with deferred, the load reads most of it itself. */
static void CompareHow(const char *name, const unsigned char *data, size_t size, BOOL deferred)
{
    TextFormat expectedFormat = TextDefaultFormat();
    size_t expectedLength = 0;
    wchar_t *expected = TextDecode(data, size, &expectedFormat, &expectedLength);
    Document reference;
    DocumentInitialize(&reference);
    CHECK(expected != NULL && DocumentAdopt(&reference, expected, expectedLength), name);

    Source source = { MemAlloc(size + 1), data, 0, 0, FALSE };
    size_t available = deferred && size > TEXTLOAD_FIRST_BYTES ? TEXTLOAD_FIRST_BYTES : size;
    memcpy(source.buffer, data, available);
    TextFormat format = TextDefaultFormat();
    TextLoad *load = TextLoadBegin(source.buffer, available, size, ReadSource, ReleaseSource, &source, &format, NULL, 0);
    CHECK(load != NULL, name);
    if (load == NULL) {
        DocumentRelease(&reference);
        MemFree(source.buffer);
        return;
    }
    Document document;
    DocumentInitialize(&document);
    DocumentAdoptLoad(&document, load);
    CHECK(DocumentLineCount(&document) >= 1, name);
    /* The first part is shown as UTF-8 until the rest is decoded; data that ends up ANSI differs there. */
    if (DocumentLength(&document) > 0 && expectedFormat.encoding != TEXT_ENCODING_ANSI) {
        CHECK(DocumentLength(&document) <= DocumentLength(&reference) && DocumentCharAt(&document, 0) == DocumentCharAt(&reference, 0), name);
    }
    DocumentStartLoad(&document);
    DocumentFinishLoad(&document);
    CHECK(source.releases == 1 && source.reads == (available < size ? 1 : 0), name);
    MemFree(source.buffer);

    CHECK(DocumentLength(&document) == DocumentLength(&reference), name);
    CHECK(DocumentLineCount(&document) == DocumentLineCount(&reference), name);
    BOOL sameText = DocumentLength(&document) == DocumentLength(&reference)
        && memcmp(DocumentText(&document), DocumentText(&reference), DocumentLength(&reference) * sizeof(wchar_t)) == 0;
    CHECK(sameText, name);
    BOOL sameLines = DocumentLineCount(&document) == DocumentLineCount(&reference);
    for (size_t line = 0; sameLines && line < DocumentLineCount(&reference); ++line) {
        sameLines = DocumentLineStart(&document, line) == DocumentLineStart(&reference, line);
    }
    CHECK(sameLines, name);
    CHECK(format.encoding == expectedFormat.encoding && format.byteOrderMark == expectedFormat.byteOrderMark
        && format.lineEnding == expectedFormat.lineEnding, name);

    DocumentRelease(&document);
    DocumentRelease(&reference);
}

static void Compare(const char *name, const unsigned char *data, size_t size)
{
    CompareHow(name, data, size, FALSE);
    CompareHow(name, data, size, TRUE);
}

/* UTF-16 units valid UTF-8 without four-byte sequences decodes to, with CRLF pairs becoming one unit. */
static size_t ExpectedUnits(const unsigned char *data, size_t size)
{
    size_t units = 0;
    for (size_t i = 0; i < size; ++i) {
        units += (data[i] & 0xC0) != 0x80;
        units -= data[i] == '\n' && i > 0 && data[i - 1] == '\r';
    }
    return units;
}

static unsigned char *Repeat(const char *pattern, size_t patternLength, const char *prefix, size_t size)
{
    unsigned char *data = MemAlloc(size + 1);
    size_t prefixLength = strlen(prefix);
    memcpy(data, prefix, prefixLength);
    for (size_t i = prefixLength; i < size; ++i) {
        data[i] = (unsigned char)pattern[(i - prefixLength) % patternLength];
    }
    return data;
}

static void CompareRepeated(const char *name, const char *pattern, const char *prefix, size_t size)
{
    size_t patternLength = strlen(pattern);
    unsigned char *data = Repeat(pattern, patternLength, prefix, size);
    Compare(name, data, size);
    MemFree(data);
}

/* The UTF-8 text as UTF-16, with a byte order mark when asked for. */
static unsigned char *Utf16Of(const unsigned char *utf8, size_t size, BOOL bigEndian, BOOL mark, size_t *outSize)
{
    int units = MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, (int)size, NULL, 0);
    wchar_t *text = MemAlloc((size_t)units * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, (int)size, text, units);
    size_t markSize = mark ? 2 : 0;
    unsigned char *bytes = MemAlloc(markSize + (size_t)units * 2);
    if (mark) {
        bytes[0] = bigEndian ? 0xFE : 0xFF;
        bytes[1] = bigEndian ? 0xFF : 0xFE;
    }
    for (int i = 0; i < units; ++i) {
        bytes[markSize + 2 * i] = bigEndian ? (unsigned char)(text[i] >> 8) : (unsigned char)text[i];
        bytes[markSize + 2 * i + 1] = bigEndian ? (unsigned char)text[i] : (unsigned char)(text[i] >> 8);
    }
    MemFree(text);
    *outSize = markSize + (size_t)units * 2;
    return bytes;
}

int wmain(void)
{
    BlockSetBudget(64 * 1024 * 1024);
    const char turkish[] = "\xC3\x87" "al\xC4\xB1\xC5\x9Ft\xC4\xB1\xC4\x9F\xC4\xB1m i\xC3\xA7in klas\xC3\xB6r ay\xC4\xB1r\xC4\xB1 bir disk. HER \xC5\x9E" "ey orada i\xC5\x9Fte, ANLATMAM gerekiyor mu? 0123456789\r\n";

    Compare("empty", (const unsigned char *)"", 0);
    CHECK(TEXTLOAD_FIRST_BYTES > 64 * 1024, "the first part covers the first chunk");
    Compare("ascii", (const unsigned char *)"abc", 3);
    Compare("mixed breaks", (const unsigned char *)"a\r\nb\rc\nd\r\r\n\n", 12);
    Compare("nul characters", (const unsigned char *)"a\0b\0\0\n", 6);
    Compare("utf-8 mark", (const unsigned char *)"\xEF\xBB\xBF" "a\r\n\xC4\x9F", 8);
    Compare("cr only", (const unsigned char *)"one\rtwo\rthree", 13);
    Compare("no break", (const unsigned char *)"single line", 11);
    Compare("three and four byte sequences", (const unsigned char *)"\xE4\xB8\xAD\xE6\x96\x87 \xF0\x9F\x98\x80 son\n", 16);
    Compare("invalid utf-8", (const unsigned char *)"abc\xC3(def\n", 9);
    Compare("overlong", (const unsigned char *)"\xC0\xAF", 2);
    Compare("surrogate in utf-8", (const unsigned char *)"\xED\xA0\x80", 3);
    Compare("truncated at end, ascii only", (const unsigned char *)"abc\xC3", 4);
    Compare("truncated at end, with utf-8", (const unsigned char *)"\xC4\x9F" "abc\xE2\x82", 7);
    Compare("first chunk only", (const unsigned char *)turkish, sizeof turkish - 1);

    CompareRepeated("turkish crlf 3 MB", turkish, "", 3 * 1024 * 1024);
    CompareRepeated("ascii lf with tabs 2 MB", "\tline of text with\ttabs and words\n", "", 2 * 1024 * 1024);
    CompareRepeated("cr only 1 MB", "a line\r", "", 1024 * 1024);
    CompareRepeated("cjk and emoji 1 MB", "\xE4\xB8\xAD\xE6\x96\x87\xF0\x9F\x98\x80\n", "", 1024 * 1024);
    CompareRepeated("crlf across the first boundary", "ab\r\n", "x", 3 * 64 * 1024 + 7);
    CompareRepeated("sequence across the first boundary", "a\xC3\xA7", "xy", 3 * 64 * 1024 + 5);
    CompareRepeated("long single line 1 MB", "0123456789abcdef", "", 1024 * 1024);

    /* Invalid bytes far into the data: the whole text falls back to ANSI. */
    size_t size = 512 * 1024;
    unsigned char *data = Repeat(turkish, sizeof turkish - 1, "", size);
    data[size - 100] = 0xFF;
    Compare("invalid byte in a later chunk", data, size);
    data[size - 100] = 'a';
    data[size - 1] = 0xE2;
    Compare("truncated sequence after a large utf-8 text", data, size);
    MemFree(data);
    data = Repeat("plain ascii text\r\n", 18, "", size);
    data[size - 1] = 0xC3;
    Compare("truncated sequence after a large ascii text", data, size);
    MemFree(data);

    /* UTF-16 in both byte orders, with and without a mark, and with an odd byte at the end. */
    size_t utf8Size = 700 * 1024;
    unsigned char *utf8 = Repeat(turkish, sizeof turkish - 1, "", utf8Size);
    size_t utf16Size = 0;
    unsigned char *utf16 = Utf16Of(utf8, utf8Size, FALSE, TRUE, &utf16Size);
    Compare("utf-16 le with mark", utf16, utf16Size);
    Compare("utf-16 le with mark and odd byte", utf16, utf16Size - 1);
    MemFree(utf16);
    utf16 = Utf16Of(utf8, utf8Size, TRUE, TRUE, &utf16Size);
    Compare("utf-16 be with mark", utf16, utf16Size);
    MemFree(utf16);
    utf16 = Utf16Of(utf8, utf8Size, FALSE, FALSE, &utf16Size);
    Compare("utf-16 le without mark", utf16, utf16Size);
    MemFree(utf16);
    utf16 = Utf16Of(utf8, utf8Size, TRUE, FALSE, &utf16Size);
    Compare("utf-16 be without mark", utf16, utf16Size);
    MemFree(utf16);
    MemFree(utf8);

    /* A load that is discarded releases everything, whether or not it was started. */
    size = 2 * 1024 * 1024;
    utf8 = Repeat(turkish, sizeof turkish - 1, "", size);
    TextFormat format = TextDefaultFormat();
    int releases = 0;
    TextLoad *load = TextLoadBegin(utf8, size, size, NULL, CountRelease, &releases, &format, NULL, 0);
    CHECK(load != NULL && format.encoding == TEXT_ENCODING_UTF8 && !format.byteOrderMark, "a load reports its format at once");
    if (load != NULL) {
        CHECK(!TextLoadIsComplete(load) && load->lineCount > 1 && load->length < size, "the first part is decoded before the start");
        TextLoadStart(load);
        TextLoadWait(load);
        CHECK(TextLoadIsComplete(load) && format.lineEnding == LINE_ENDING_CRLF && load->length == ExpectedUnits(utf8, size),
            "waiting completes the load");
        TextLoadDiscard(load);
    }
    CHECK(releases == 1, "discarding releases the data");
    load = TextLoadBegin(utf8, size, size, NULL, CountRelease, &releases, &format, NULL, 0);
    if (load != NULL) {
        TextLoadStart(load);
        TextLoadDiscard(load);
    }
    CHECK(releases == 2, "discarding a started load waits and releases the data");
    load = TextLoadBegin(utf8, size, size, NULL, CountRelease, &releases, &format, NULL, 0);
    if (load != NULL) {
        TextLoadDiscard(load);
    }
    CHECK(releases == 3, "discarding a load that was never started releases the data");

    /* A failing read leaves the first part only. */
    Source failing = { MemAlloc(size), utf8, 0, 0, TRUE };
    memcpy(failing.buffer, utf8, TEXTLOAD_FIRST_BYTES);
    load = TextLoadBegin(failing.buffer, TEXTLOAD_FIRST_BYTES, size, ReadSource, ReleaseSource, &failing, &format, NULL, 0);
    CHECK(load != NULL, "a deferred load begins");
    if (load != NULL) {
        size_t firstLength = load->length;
        TextLoadStart(load);
        TextLoadWait(load);
        CHECK(load->error == ERROR_READ_FAULT && load->length == firstLength && failing.reads == 1, "a read error keeps the first part");
        TextLoadDiscard(load);
    }
    MemFree(failing.buffer);
    MemFree(utf8);

    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
