#include "text.h"
#include "quickpad.h"

#include <emmintrin.h>

/* MultiByteToWideChar and WideCharToMultiByte take int lengths. */
#define TEXT_MAX_LENGTH 0x7FFFFFF0u

TextFormat TextDefaultFormat(void)
{
    TextFormat format = { TEXT_ENCODING_UTF8, FALSE, LINE_ENDING_CRLF };
    return format;
}

/* Text without a byte order mark counts as UTF-16 when most units of a sample have a zero byte on the same side. */
static BOOL LooksLikeUtf16(const unsigned char *data, size_t size, TextEncoding *encoding)
{
    size_t sample = (size < 4096 ? size : 4096) & ~(size_t)1;
    size_t units = sample / 2;
    if (units == 0) {
        return FALSE;
    }

    size_t zeroLow = 0;
    size_t zeroHigh = 0;
    for (size_t i = 0; i < sample; i += 2) {
        zeroHigh += data[i] == 0;
        zeroLow += data[i + 1] == 0;
    }

    if (zeroLow * 2 > units && zeroHigh * 8 < zeroLow) {
        *encoding = TEXT_ENCODING_UTF16LE;
        return TRUE;
    }
    if (zeroHigh * 2 > units && zeroLow * 8 < zeroHigh) {
        *encoding = TEXT_ENCODING_UTF16BE;
        return TRUE;
    }
    return FALSE;
}

/* Number of bytes at the end that start a UTF-8 sequence the file does not complete. */
static size_t TrailingIncompleteUtf8(const unsigned char *data, size_t size)
{
    for (size_t back = 1; back <= 3 && back <= size; ++back) {
        unsigned char c = data[size - back];
        if ((c & 0xC0) == 0x80) {
            continue;
        }
        size_t sequence = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        return sequence > back ? back : 0;
    }
    return 0;
}

/*
 * One UTF-16 unit per input byte is always enough for UTF-8 and for the ANSI code pages.
 * Two extra units leave room for a replacement character and the terminator.
 * Returns NULL with ERROR_NO_UNICODE_TRANSLATION when flags reject the input.
 */
static wchar_t *DecodeMultiByte(UINT codePage, DWORD flags, const unsigned char *data, size_t size, size_t *length)
{
    wchar_t *text = MemAlloc((size + 2) * sizeof(wchar_t));
    if (text == NULL) {
        return NULL;
    }

    int written = 0;
    if (size > 0) {
        written = MultiByteToWideChar(codePage, flags, (const char *)data, (int)size, text, (int)size);
        if (written == 0) {
            DWORD error = GetLastError();
            MemFree(text);
            SetLastError(error);
            return NULL;
        }
    }
    text[written] = 0;
    *length = (size_t)written;
    return text;
}

static wchar_t *DecodeUtf16(const unsigned char *data, size_t size, BOOL bigEndian, size_t *length)
{
    size_t units = size / 2;
    wchar_t *text = MemAlloc((units + 2) * sizeof(wchar_t));
    if (text == NULL) {
        return NULL;
    }

    memcpy(text, data, units * sizeof(wchar_t));
    if (bigEndian) {
        for (size_t i = 0; i < units; ++i) {
            text[i] = (wchar_t)((text[i] << 8) | (text[i] >> 8));
        }
    }
    if (size % 2 != 0) {
        text[units++] = 0xFFFD;
    }
    text[units] = 0;
    *length = units;
    return text;
}

static void ReplaceNulCharacters(wchar_t *text, size_t length)
{
    const __m128i zero = _mm_setzero_si128();
    size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        __m128i block = _mm_loadu_si128((const __m128i *)(text + i));
        if (_mm_movemask_epi8(_mm_cmpeq_epi16(block, zero)) != 0) {
            for (size_t k = i; k < i + 8; ++k) {
                if (text[k] == 0) {
                    text[k] = L' ';
                }
            }
        }
    }
    for (; i < length; ++i) {
        if (text[i] == 0) {
            text[i] = L' ';
        }
    }
}

/* The first line break decides; text without one gets CRLF. */
static LineEnding DetectLineEnding(const wchar_t *text, size_t length)
{
    const __m128i cr = _mm_set1_epi16(L'\r');
    const __m128i lf = _mm_set1_epi16(L'\n');
    size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        __m128i block = _mm_loadu_si128((const __m128i *)(text + i));
        __m128i breaks = _mm_or_si128(_mm_cmpeq_epi16(block, cr), _mm_cmpeq_epi16(block, lf));
        if (_mm_movemask_epi8(breaks) != 0) {
            break;
        }
    }
    for (; i < length; ++i) {
        if (text[i] == L'\n') {
            return LINE_ENDING_LF;
        }
        if (text[i] == L'\r') {
            return i + 1 < length && text[i + 1] == L'\n' ? LINE_ENDING_CRLF : LINE_ENDING_CR;
        }
    }
    return LINE_ENDING_CRLF;
}

wchar_t *TextDecode(const unsigned char *data, size_t size, TextFormat *format, size_t *length)
{
    if (size > TEXT_MAX_LENGTH) {
        return NULL;
    }

    TextFormat detected = TextDefaultFormat();
    TextEncoding utf16 = TEXT_ENCODING_UTF16LE;
    wchar_t *text = NULL;
    size_t count = 0;

    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        detected.byteOrderMark = TRUE;
        text = DecodeMultiByte(CP_UTF8, 0, data + 3, size - 3, &count);
    } else if (size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        detected.encoding = TEXT_ENCODING_UTF16LE;
        detected.byteOrderMark = TRUE;
        text = DecodeUtf16(data + 2, size - 2, FALSE, &count);
    } else if (size >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        detected.encoding = TEXT_ENCODING_UTF16BE;
        detected.byteOrderMark = TRUE;
        text = DecodeUtf16(data + 2, size - 2, TRUE, &count);
    } else if (LooksLikeUtf16(data, size, &utf16)) {
        detected.encoding = utf16;
        text = DecodeUtf16(data, size, utf16 == TEXT_ENCODING_UTF16BE, &count);
    } else {
        size_t incomplete = TrailingIncompleteUtf8(data, size);
        SetLastError(ERROR_SUCCESS);
        text = DecodeMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, data, size - incomplete, &count);
        BOOL valid = text != NULL;
        if (valid && incomplete > 0 && count == size - incomplete) {
            /* Only ASCII before a cut-off sequence is no evidence of UTF-8: an ANSI letter can look like a lead byte. */
            MemFree(text);
            text = NULL;
            valid = FALSE;
        } else if (valid && incomplete > 0) {
            text[count++] = 0xFFFD;
            text[count] = 0;
        }
        if (!valid && (incomplete > 0 || GetLastError() == ERROR_NO_UNICODE_TRANSLATION)) {
            detected.encoding = TEXT_ENCODING_ANSI;
            text = DecodeMultiByte(CP_ACP, 0, data, size, &count);
        }
    }

    if (text == NULL) {
        return NULL;
    }
    ReplaceNulCharacters(text, count);
    detected.lineEnding = DetectLineEnding(text, count);
    *format = detected;
    *length = count;
    return text;
}

/* Returns text itself when it already uses the line ending, otherwise a converted copy stored in *copy. */
static const wchar_t *NormalizeLineEndings(const wchar_t *text, size_t length, LineEnding ending,
    wchar_t **copy, size_t *resultLength)
{
    const __m128i cr = _mm_set1_epi16(L'\r');
    const __m128i lf = _mm_set1_epi16(L'\n');
    size_t pairs = 0;
    size_t lonelyCr = 0;
    size_t lonelyLf = 0;

    size_t i = 0;
    while (i < length) {
        if (i + 8 <= length) {
            __m128i block = _mm_loadu_si128((const __m128i *)(text + i));
            __m128i breaks = _mm_or_si128(_mm_cmpeq_epi16(block, cr), _mm_cmpeq_epi16(block, lf));
            if (_mm_movemask_epi8(breaks) == 0) {
                i += 8;
                continue;
            }
        }
        if (text[i] == L'\r') {
            if (i + 1 < length && text[i + 1] == L'\n') {
                ++pairs;
                ++i;
            } else {
                ++lonelyCr;
            }
        } else if (text[i] == L'\n') {
            ++lonelyLf;
        }
        ++i;
    }

    BOOL conforming = ending == LINE_ENDING_CRLF ? lonelyCr == 0 && lonelyLf == 0
        : ending == LINE_ENDING_LF ? pairs == 0 && lonelyCr == 0
        : pairs == 0 && lonelyLf == 0;
    if (conforming) {
        *copy = NULL;
        *resultLength = length;
        return text;
    }

    size_t breakCount = pairs + lonelyCr + lonelyLf;
    size_t converted = length - pairs + (ending == LINE_ENDING_CRLF ? breakCount : 0);
    wchar_t *output = MemAlloc((converted + 1) * sizeof(wchar_t));
    if (output == NULL) {
        return NULL;
    }

    size_t out = 0;
    for (i = 0; i < length; ++i) {
        wchar_t c = text[i];
        if (c != L'\r' && c != L'\n') {
            output[out++] = c;
            continue;
        }
        if (c == L'\r' && i + 1 < length && text[i + 1] == L'\n') {
            ++i;
        }
        if (ending == LINE_ENDING_CRLF) {
            output[out++] = L'\r';
            output[out++] = L'\n';
        } else {
            output[out++] = ending == LINE_ENDING_LF ? L'\n' : L'\r';
        }
    }
    output[out] = 0;
    *copy = output;
    *resultLength = out;
    return output;
}

static unsigned char *EncodeMultiByte(UINT codePage, const wchar_t *text, size_t length, BOOL byteOrderMark,
    size_t *size, BOOL *lossy)
{
    /* The ANSI code page can be UTF-8, which rejects the lossy-conversion arguments. */
    BOOL utf8 = codePage == CP_UTF8 || (codePage == CP_ACP && GetACP() == CP_UTF8);
    DWORD flags = utf8 ? 0 : WC_NO_BEST_FIT_CHARS;
    BOOL usedDefault = FALSE;
    BOOL *usedDefaultArgument = utf8 ? NULL : &usedDefault;

    int needed = 0;
    if (length > 0) {
        needed = WideCharToMultiByte(codePage, flags, text, (int)length, NULL, 0, NULL, usedDefaultArgument);
        if (needed == 0) {
            return NULL;
        }
    }

    size_t markSize = byteOrderMark ? 3 : 0;
    unsigned char *bytes = MemAlloc(markSize + (size_t)needed + 1);
    if (bytes == NULL) {
        return NULL;
    }
    if (byteOrderMark) {
        bytes[0] = 0xEF;
        bytes[1] = 0xBB;
        bytes[2] = 0xBF;
    }
    if (length > 0) {
        WideCharToMultiByte(codePage, flags, text, (int)length, (char *)bytes + markSize, needed, NULL, usedDefaultArgument);
    }
    *size = markSize + (size_t)needed;
    *lossy = usedDefault;
    return bytes;
}

static unsigned char *EncodeUtf16(const wchar_t *text, size_t length, BOOL bigEndian, BOOL byteOrderMark, size_t *size)
{
    size_t markSize = byteOrderMark ? 2 : 0;
    unsigned char *bytes = MemAlloc(markSize + length * sizeof(wchar_t) + 1);
    if (bytes == NULL) {
        return NULL;
    }
    if (byteOrderMark) {
        bytes[0] = bigEndian ? 0xFE : 0xFF;
        bytes[1] = bigEndian ? 0xFF : 0xFE;
    }

    unsigned char *out = bytes + markSize;
    if (bigEndian) {
        for (size_t i = 0; i < length; ++i) {
            out[2 * i] = (unsigned char)(text[i] >> 8);
            out[2 * i + 1] = (unsigned char)text[i];
        }
    } else {
        memcpy(out, text, length * sizeof(wchar_t));
    }
    *size = markSize + length * sizeof(wchar_t);
    return bytes;
}

unsigned char *TextEncode(const wchar_t *text, size_t length, TextFormat format, size_t *size, BOOL *lossy)
{
    *size = 0;
    *lossy = FALSE;

    wchar_t *copy = NULL;
    size_t sourceLength = 0;
    const wchar_t *source = NormalizeLineEndings(text, length, format.lineEnding, &copy, &sourceLength);
    if (source == NULL || sourceLength > TEXT_MAX_LENGTH) {
        MemFree(copy);
        return NULL;
    }

    unsigned char *bytes = NULL;
    switch (format.encoding) {
    case TEXT_ENCODING_UTF16LE:
    case TEXT_ENCODING_UTF16BE:
        bytes = EncodeUtf16(source, sourceLength, format.encoding == TEXT_ENCODING_UTF16BE, format.byteOrderMark, size);
        break;
    case TEXT_ENCODING_ANSI:
        bytes = EncodeMultiByte(CP_ACP, source, sourceLength, FALSE, size, lossy);
        break;
    default:
        bytes = EncodeMultiByte(CP_UTF8, source, sourceLength, format.byteOrderMark, size, lossy);
        break;
    }

    MemFree(copy);
    return bytes;
}
