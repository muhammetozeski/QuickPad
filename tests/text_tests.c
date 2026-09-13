/* Unit tests for src/text.c: format detection, decoding and round trips back to the original bytes. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "text.h"
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

/* Decodes bytes, checks the detected format and text, then encodes back and expects the same bytes. */
static void ExpectRoundTrip(const char *name, const unsigned char *bytes, size_t size, TextEncoding encoding,
    BOOL byteOrderMark, LineEnding ending, const wchar_t *expected, size_t expectedLength)
{
    TextFormat format;
    size_t length = 0;
    wchar_t *text = TextDecode(bytes, size, &format, &length);
    CHECK(text != NULL, name);
    if (text == NULL) {
        return;
    }
    CHECK(format.encoding == encoding, name);
    CHECK(format.byteOrderMark == byteOrderMark, name);
    CHECK(format.lineEnding == ending, name);
    CHECK(length == expectedLength, name);
    CHECK(length == expectedLength && wmemcmp(text, expected, length) == 0, name);
    CHECK(text[length] == 0, name);

    size_t encodedSize = 0;
    BOOL lossy = TRUE;
    unsigned char *encoded = TextEncode(text, length, format, &encodedSize, &lossy);
    CHECK(encoded != NULL, name);
    CHECK(!lossy, name);
    CHECK(encodedSize == size, name);
    CHECK(encoded != NULL && encodedSize == size && memcmp(encoded, bytes, size) == 0, name);

    MemFree(encoded);
    MemFree(text);
}

static void ExpectEncoding(const char *name, const wchar_t *text, TextFormat format, const unsigned char *expected,
    size_t expectedSize)
{
    size_t size = 0;
    BOOL lossy = TRUE;
    unsigned char *bytes = TextEncode(text, wcslen(text), format, &size, &lossy);
    CHECK(bytes != NULL, name);
    CHECK(!lossy, name);
    CHECK(size == expectedSize, name);
    CHECK(bytes != NULL && size == expectedSize && memcmp(bytes, expected, size) == 0, name);
    MemFree(bytes);
}

#define BYTES(...) ((const unsigned char[]){ __VA_ARGS__ })
#define ROUND_TRIP(name, encoding, mark, ending, expected, ...)                                     \
    ExpectRoundTrip(name, BYTES(__VA_ARGS__), sizeof(BYTES(__VA_ARGS__)), encoding, mark, ending,  \
        expected, wcslen(expected))

static BOOL AnsiIsSingleByteWestern(void)
{
    UINT codePage = GetACP();
    return codePage >= 1250 && codePage <= 1258;
}

int wmain(void)
{
    ROUND_TRIP("utf8 with mark, crlf", TEXT_ENCODING_UTF8, TRUE, LINE_ENDING_CRLF, L"a\r\n\x011F",
        0xEF, 0xBB, 0xBF, 'a', 0x0D, 0x0A, 0xC4, 0x9F);
    ROUND_TRIP("utf8 without mark, lf", TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_LF, L"x\ny\n\x20AC",
        'x', 0x0A, 'y', 0x0A, 0xE2, 0x82, 0xAC);
    ROUND_TRIP("ascii, cr", TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_CR, L"one\rtwo",
        'o', 'n', 'e', 0x0D, 't', 'w', 'o');
    ROUND_TRIP("single line gets crlf", TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_CRLF, L"hello",
        'h', 'e', 'l', 'l', 'o');
    ROUND_TRIP("utf16le with mark, crlf", TEXT_ENCODING_UTF16LE, TRUE, LINE_ENDING_CRLF, L"h\r\n\x011F",
        0xFF, 0xFE, 'h', 0x00, 0x0D, 0x00, 0x0A, 0x00, 0x1F, 0x01);
    ROUND_TRIP("utf16be with mark, lf", TEXT_ENCODING_UTF16BE, TRUE, LINE_ENDING_LF, L"h\n\x011F",
        0xFE, 0xFF, 0x00, 'h', 0x00, 0x0A, 0x01, 0x1F);
    ROUND_TRIP("utf16le without mark", TEXT_ENCODING_UTF16LE, FALSE, LINE_ENDING_CRLF, L"ab\r\nc",
        'a', 0x00, 'b', 0x00, 0x0D, 0x00, 0x0A, 0x00, 'c', 0x00);
    ROUND_TRIP("utf16be without mark", TEXT_ENCODING_UTF16BE, FALSE, LINE_ENDING_LF, L"ab\nc",
        0x00, 'a', 0x00, 'b', 0x00, 0x0A, 0x00, 'c');
    ROUND_TRIP("crlf across a block boundary", TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_CRLF, L"1234567\r\n89",
        '1', '2', '3', '4', '5', '6', '7', 0x0D, 0x0A, '8', '9');
    ROUND_TRIP("utf8 mark only", TEXT_ENCODING_UTF8, TRUE, LINE_ENDING_CRLF, L"", 0xEF, 0xBB, 0xBF);
    ExpectRoundTrip("empty", (const unsigned char *)"", 0, TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_CRLF, L"", 0);

    if (AnsiIsSingleByteWestern()) {
        const unsigned char ansi[] = { 'c', 'a', 'f', 0xE9, 0x0D, 0x0A, 0xF6 };
        wchar_t expected[16];
        int expectedLength = MultiByteToWideChar(CP_ACP, 0, (const char *)ansi, sizeof ansi, expected, 16);
        ExpectRoundTrip("invalid utf8 is ansi", ansi, sizeof ansi, TEXT_ENCODING_ANSI, FALSE, LINE_ENDING_CRLF,
            expected, (size_t)expectedLength);

        const unsigned char overlong[] = { 'a', 0xC0, 0x80, 'b' };
        expectedLength = MultiByteToWideChar(CP_ACP, 0, (const char *)overlong, sizeof overlong, expected, 16);
        ExpectRoundTrip("overlong utf8 is ansi", overlong, sizeof overlong, TEXT_ENCODING_ANSI, FALSE,
            LINE_ENDING_CRLF, expected, (size_t)expectedLength);

        const unsigned char trailingLetter[] = { 'a', 'b', 0xE7 };
        expectedLength = MultiByteToWideChar(CP_ACP, 0, (const char *)trailingLetter, sizeof trailingLetter, expected, 16);
        ExpectRoundTrip("ascii with one trailing ansi letter is ansi", trailingLetter, sizeof trailingLetter,
            TEXT_ENCODING_ANSI, FALSE, LINE_ENDING_CRLF, expected, (size_t)expectedLength);

        TextFormat ansiFormat = { TEXT_ENCODING_ANSI, FALSE, LINE_ENDING_CRLF };
        size_t size = 0;
        BOOL lossy = FALSE;
        unsigned char *bytes = TextEncode(L"a\x4E2D", 2, ansiFormat, &size, &lossy);
        CHECK(bytes != NULL && lossy, "unmappable ansi character is reported");
        MemFree(bytes);
    } else {
        wprintf(L"SKIP ansi tests: code page %u\n", GetACP());
    }

    {
        TextFormat format;
        size_t length = 0;
        const unsigned char truncated[] = { 0xC4, 0x9F, 'a', 0xC4 };
        wchar_t *text = TextDecode(truncated, sizeof truncated, &format, &length);
        CHECK(text != NULL && format.encoding == TEXT_ENCODING_UTF8, "truncated utf8 stays utf8");
        CHECK(text != NULL && length == 3 && wmemcmp(text, L"\x011F" L"a\xFFFD", 3) == 0, "truncated utf8 ends with U+FFFD");
        MemFree(text);

        const unsigned char nul[] = { 'a', 'b', 'c', 0x00, 'd', 'e', 'f', 0x0A };
        text = TextDecode(nul, sizeof nul, &format, &length);
        CHECK(text != NULL && format.encoding == TEXT_ENCODING_UTF8, "nul in utf8 stays utf8");
        CHECK(text != NULL && length == 8 && wmemcmp(text, L"abc def\n", 8) == 0, "nul becomes a space");
        MemFree(text);
    }

    {
        TextFormat crlf = { TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_CRLF };
        ExpectEncoding("rich edit breaks to crlf", L"a\rb\r", crlf, BYTES('a', 0x0D, 0x0A, 'b', 0x0D, 0x0A), 6);

        TextFormat lf = { TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_LF };
        ExpectEncoding("mixed breaks to lf", L"a\r\nb\nc\rd", lf, BYTES('a', 0x0A, 'b', 0x0A, 'c', 0x0A, 'd'), 7);

        TextFormat cr16 = { TEXT_ENCODING_UTF16BE, TRUE, LINE_ENDING_CR };
        ExpectEncoding("utf16be with mark and cr", L"a\r\nb", cr16, BYTES(0xFE, 0xFF, 0x00, 'a', 0x00, 0x0D, 0x00, 'b'), 8);
    }

    {
        /* Long enough to go through the vector loops on both sides of the conversion. */
        size_t lines = 5000;
        size_t size = lines * 10;
        unsigned char *bytes = MemAlloc(size);
        wchar_t *expected = MemAlloc(size * sizeof(wchar_t));
        for (size_t i = 0; i < size; ++i) {
            unsigned char c = i % 10 == 9 ? 0x0A : (unsigned char)('a' + i % 10);
            bytes[i] = c;
            expected[i] = c;
        }
        ExpectRoundTrip("long lf text", bytes, size, TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_LF, expected, size);
        MemFree(expected);
        MemFree(bytes);
    }

    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
