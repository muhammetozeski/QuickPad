#include "textload.h"
#include "blocks.h"
#include "quickpad.h"

#include <emmintrin.h>
#include <intrin.h>

/* The part decoded before the load is handed out; a screen of text is a few kilobytes. */
#define FIRST_CHUNK (64 * 1024)
#define CHUNK_MINIMUM (128 * 1024)
#define CHUNK_MAXIMUM (4 * 1024 * 1024)
#define CHUNKS_PER_WORKER 4
#define TEXT_SLACK 4096

/* ---- Counting ------------------------------------------------------------------------------ */

static unsigned PopCount16(unsigned value)
{
    return (unsigned)__popcnt(value);
}

/* Units and line breaks a UTF-8 chunk decodes to, exact when it is valid; the decoder checks that. */
static void CountUtf8(const unsigned char *bytes, size_t size, TextLoadChunk *chunk)
{
    const __m128i continuationLimit = _mm_set1_epi8(-64);   /* 0x80..0xBF are below this as signed bytes */
    const __m128i fourByteLimit = _mm_set1_epi8(-17);       /* 0xF0..0xFF are negative and above this */
    const __m128i cr = _mm_set1_epi8('\r');
    const __m128i lf = _mm_set1_epi8('\n');
    size_t nonContinuation = 0;
    size_t fourByte = 0;
    size_t lfCount = 0;
    size_t crCount = 0;
    size_t crlfCount = 0;
    unsigned nonAscii = 0;
    unsigned previousCr = 0;
    size_t i = 0;
    for (; i + 16 <= size; i += 16) {
        __m128i block = _mm_loadu_si128((const __m128i *)(bytes + i));
        unsigned negative = (unsigned)_mm_movemask_epi8(block);
        unsigned continuation = (unsigned)_mm_movemask_epi8(_mm_cmplt_epi8(block, continuationLimit));
        unsigned lead4 = (unsigned)_mm_movemask_epi8(_mm_cmpgt_epi8(block, fourByteLimit)) & negative;
        unsigned crMask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block, cr));
        unsigned lfMask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block, lf));
        nonAscii |= negative;
        nonContinuation += 16 - PopCount16(continuation);
        fourByte += PopCount16(lead4);
        crCount += PopCount16(crMask);
        lfCount += PopCount16(lfMask);
        crlfCount += PopCount16(((crMask << 1) | previousCr) & lfMask);
        previousCr = crMask >> 15;
    }
    for (; i < size; ++i) {
        unsigned char c = bytes[i];
        nonAscii |= c >= 0x80;
        nonContinuation += (c & 0xC0) != 0x80;
        fourByte += c >= 0xF0;
        crCount += c == '\r';
        lfCount += c == '\n';
        crlfCount += c == '\n' && previousCr;
        previousCr = c == '\r';
    }
    chunk->units = nonContinuation + fourByte - crlfCount;
    chunk->breaks = lfCount + crCount - crlfCount;
    chunk->nonAscii = nonAscii != 0;
}

static unsigned Unit16(const unsigned char *bytes, size_t index, BOOL bigEndian)
{
    return bigEndian ? (unsigned)(bytes[index] << 8) | bytes[index + 1] : (unsigned)(bytes[index + 1] << 8) | bytes[index];
}

static void CountUtf16(const unsigned char *bytes, size_t size, BOOL bigEndian, TextLoadChunk *chunk)
{
    size_t units = size / 2;
    size_t crlfCount = 0;
    size_t breaks = 0;
    unsigned previousCr = 0;
    for (size_t i = 0; i < units; ++i) {
        unsigned unit = Unit16(bytes, 2 * i, bigEndian);
        crlfCount += unit == '\n' && previousCr;
        breaks += unit == '\n' || unit == '\r';
        previousCr = unit == '\r';
    }
    chunk->units = units - crlfCount;
    chunk->breaks = breaks - crlfCount;
    chunk->nonAscii = TRUE;
}

/* ---- Decoding ------------------------------------------------------------------------------ */

typedef struct Decoded {
    size_t written;
    size_t lines;
    BOOL invalid;
} Decoded;

/*
 * UTF-8 to UTF-16 with CRLF and CR turned into LF, NUL into space, and the absolute position after
 * every line break written to lineStarts. Malformed input, or more output than capacity, stops
 * the chunk with invalid set.
 */
static Decoded DecodeUtf8(const unsigned char *bytes, size_t size, wchar_t *out, size_t capacity, size_t outBase, size_t *lineStarts)
{
    Decoded result = { 0 };
    const __m128i zero = _mm_setzero_si128();
    const __m128i cr = _mm_set1_epi8('\r');
    const __m128i lf = _mm_set1_epi8('\n');
    wchar_t *o = out;
    wchar_t *end = out + capacity;
    size_t lines = 0;
    size_t i = 0;
    while (i < size) {
        if (i + 16 <= size && o + 16 <= end) {
            __m128i block = _mm_loadu_si128((const __m128i *)(bytes + i));
            unsigned negative = (unsigned)_mm_movemask_epi8(block);
            unsigned special = (unsigned)_mm_movemask_epi8(_mm_or_si128(_mm_cmpeq_epi8(block, cr), _mm_cmpeq_epi8(block, zero)));
            if ((negative | special) == 0) {
                _mm_storeu_si128((__m128i *)o, _mm_unpacklo_epi8(block, zero));
                _mm_storeu_si128((__m128i *)(o + 8), _mm_unpackhi_epi8(block, zero));
                unsigned lfMask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(block, lf));
                while (lfMask != 0) {
                    unsigned long bit;
                    _BitScanForward(&bit, lfMask);
                    lineStarts[lines++] = outBase + (size_t)(o - out) + bit + 1;
                    lfMask &= lfMask - 1;
                }
                o += 16;
                i += 16;
                continue;
            }
        }
        unsigned c = bytes[i];
        if (c < 0x80) {
            if (o >= end) {
                result.invalid = TRUE;
                break;
            }
            if (c == '\r') {
                *o++ = L'\n';
                lineStarts[lines++] = outBase + (size_t)(o - out);
                ++i;
                if (i < size && bytes[i] == '\n') {
                    ++i;
                }
            } else if (c == '\n') {
                *o++ = L'\n';
                lineStarts[lines++] = outBase + (size_t)(o - out);
                ++i;
            } else {
                *o++ = (wchar_t)(c == 0 ? ' ' : c);
                ++i;
            }
            continue;
        }
        size_t remaining = size - i;
        if (c >= 0xC2 && c <= 0xDF) {
            if (remaining < 2 || (bytes[i + 1] & 0xC0) != 0x80 || o >= end) {
                result.invalid = TRUE;
                break;
            }
            *o++ = (wchar_t)(((c & 0x1F) << 6) | (bytes[i + 1] & 0x3F));
            i += 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (remaining < 3 || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80 || o >= end
                || (c == 0xE0 && bytes[i + 1] < 0xA0) || (c == 0xED && bytes[i + 1] >= 0xA0)) {
                result.invalid = TRUE;
                break;
            }
            *o++ = (wchar_t)(((c & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F));
            i += 3;
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (remaining < 4 || (bytes[i + 1] & 0xC0) != 0x80 || (bytes[i + 2] & 0xC0) != 0x80 || (bytes[i + 3] & 0xC0) != 0x80
                || o + 2 > end || (c == 0xF0 && bytes[i + 1] < 0x90) || (c == 0xF4 && bytes[i + 1] >= 0x90)) {
                result.invalid = TRUE;
                break;
            }
            unsigned codePoint = ((c & 0x07) << 18) | ((bytes[i + 1] & 0x3F) << 12) | ((bytes[i + 2] & 0x3F) << 6) | (bytes[i + 3] & 0x3F);
            codePoint -= 0x10000;
            *o++ = (wchar_t)(0xD800 + (codePoint >> 10));
            *o++ = (wchar_t)(0xDC00 + (codePoint & 0x3FF));
            i += 4;
        } else {
            result.invalid = TRUE;
            break;
        }
    }
    result.written = (size_t)(o - out);
    result.lines = lines;
    return result;
}

static Decoded DecodeUtf16(const unsigned char *bytes, size_t size, BOOL bigEndian, wchar_t *out, size_t capacity, size_t outBase, size_t *lineStarts)
{
    Decoded result = { 0 };
    size_t units = size / 2;
    wchar_t *o = out;
    size_t lines = 0;
    for (size_t i = 0; i < units; ++i) {
        wchar_t unit = (wchar_t)Unit16(bytes, 2 * i, bigEndian);
        if ((size_t)(o - out) >= capacity) {
            result.invalid = TRUE;
            break;
        }
        if (unit == L'\r') {
            *o++ = L'\n';
            lineStarts[lines++] = outBase + (size_t)(o - out);
            if (i + 1 < units && Unit16(bytes, 2 * i + 2, bigEndian) == '\n') {
                ++i;
            }
        } else if (unit == L'\n') {
            *o++ = L'\n';
            lineStarts[lines++] = outBase + (size_t)(o - out);
        } else {
            *o++ = unit == 0 ? L' ' : unit;
        }
    }
    result.written = (size_t)(o - out);
    result.lines = lines;
    return result;
}

/* CR and CRLF to LF, NUL to space, line starts from position 1 on; returns the new length and sets *lineCount. */
static size_t NormalizeAndIndex(wchar_t *text, size_t length, size_t *lineStarts, size_t *lineCount)
{
    size_t write = 0;
    size_t lines = 1;
    lineStarts[0] = 0;
    for (size_t read = 0; read < length; ++read) {
        wchar_t c = text[read];
        if (c == L'\r') {
            text[write++] = L'\n';
            lineStarts[lines++] = write;
            if (read + 1 < length && text[read + 1] == L'\n') {
                ++read;
            }
        } else if (c == L'\n') {
            text[write++] = L'\n';
            lineStarts[lines++] = write;
        } else {
            text[write++] = c == 0 ? L' ' : c;
        }
    }
    *lineCount = lines;
    return write;
}

/* ---- Chunks -------------------------------------------------------------------------------- */

/* Moves a boundary forward, up to limit, so it splits neither a UTF-8 sequence nor a CRLF pair. */
static size_t AdjustUtf8Boundary(const unsigned char *bytes, size_t limit, size_t boundary)
{
    while (boundary < limit && ((bytes[boundary] & 0xC0) == 0x80 || (bytes[boundary] == '\n' && bytes[boundary - 1] == '\r'))) {
        ++boundary;
    }
    return boundary;
}

static size_t AdjustUtf16Boundary(const unsigned char *bytes, size_t limit, size_t boundary, BOOL bigEndian)
{
    boundary &= ~(size_t)1;
    while (boundary + 1 < limit && Unit16(bytes, boundary, bigEndian) == '\n' && Unit16(bytes, boundary - 2, bigEndian) == '\r') {
        boundary += 2;
    }
    return boundary < limit ? boundary : limit;
}

static size_t Boundary(const TextLoad *load, size_t limit, size_t boundary)
{
    if (boundary >= limit) {
        return limit;
    }
    return load->encoding == TEXT_ENCODING_UTF8 ? AdjustUtf8Boundary(load->data, limit, boundary)
                                                : AdjustUtf16Boundary(load->data, limit, boundary, load->encoding == TEXT_ENCODING_UTF16BE);
}

/* Splits [start, bodyEnd) into chunks of about piece bytes after the ones already made. */
static BOOL AddChunks(TextLoad *load, size_t start, size_t piece)
{
    while (start < load->bodyEnd) {
        if (load->chunkCount == load->chunkCapacity) {
            size_t capacity = load->chunkCapacity * 2 + 16;
            TextLoadChunk *chunks = MemAllocZero(capacity * sizeof(TextLoadChunk));
            if (chunks == NULL) {
                return FALSE;
            }
            memcpy(chunks, load->chunks, load->chunkCount * sizeof(TextLoadChunk));
            MemFree(load->chunks);
            load->chunks = chunks;
            load->chunkCapacity = capacity;
        }
        size_t next = Boundary(load, load->bodyEnd, start + piece);
        load->chunks[load->chunkCount].start = start;
        load->chunks[load->chunkCount].end = next;
        ++load->chunkCount;
        start = next;
    }
    return TRUE;
}

static size_t PieceSize(const TextLoad *load, size_t rest)
{
    size_t piece = rest / ((size_t)load->workers * CHUNKS_PER_WORKER + 1);
    return piece < CHUNK_MINIMUM ? CHUNK_MINIMUM : piece > CHUNK_MAXIMUM ? CHUNK_MAXIMUM : piece;
}

/* ---- Worker steps: read, count, decode ----------------------------------------------------- */

static void DecodingDone(void *context)
{
    TextLoad *load = context;
    InterlockedExchange(&load->decoded, 1);
    if (load->notifyWindow != NULL) {
        PostMessageW(load->notifyWindow, load->notifyMessage, 0, (LPARAM)load);
    }
}

static void CountChunk(void *context, size_t index)
{
    TextLoad *load = context;
    TextLoadChunk *chunk = &load->chunks[index + 1];
    if (load->encoding == TEXT_ENCODING_UTF8) {
        CountUtf8(load->data + chunk->start, chunk->end - chunk->start, chunk);
    } else {
        CountUtf16(load->data + chunk->start, chunk->end - chunk->start, load->encoding == TEXT_ENCODING_UTF16BE, chunk);
    }
}

static void DecodeChunk(void *context, size_t index)
{
    TextLoad *load = context;
    TextLoadChunk *chunk = &load->chunks[index + 1];
    Decoded decoded;
    if (load->encoding == TEXT_ENCODING_UTF8) {
        decoded = DecodeUtf8(load->data + chunk->start, chunk->end - chunk->start, load->text + chunk->outOffset, chunk->units,
            chunk->outOffset, load->lineStarts + chunk->lineOffset);
    } else {
        decoded = DecodeUtf16(load->data + chunk->start, chunk->end - chunk->start, load->encoding == TEXT_ENCODING_UTF16BE,
            load->text + chunk->outOffset, chunk->units, chunk->outOffset, load->lineStarts + chunk->lineOffset);
    }
    chunk->written = decoded.written;
    chunk->lines = decoded.lines;
    if (decoded.invalid || decoded.written != chunk->units || decoded.lines != chunk->breaks) {
        InterlockedExchange(&load->invalid, 1);
    }
}

/* Runs on the worker that counted the last chunk: places every chunk and starts decoding. */
static void CountingDone(void *context)
{
    TextLoad *load = context;
    size_t outOffset = load->chunks[0].written;
    size_t lineOffset = 1 + load->chunks[0].lines;
    for (size_t i = 1; i < load->chunkCount; ++i) {
        TextLoadChunk *chunk = &load->chunks[i];
        chunk->outOffset = outOffset;
        chunk->lineOffset = lineOffset;
        outOffset += chunk->units;
        lineOffset += chunk->breaks;
        load->anyNonAscii = load->anyNonAscii || chunk->nonAscii;
    }
    if (outOffset > load->textCapacity - 2 || lineOffset > load->lineCapacity) {
        /* Counts a valid file cannot produce; the ANSI fallback decodes it whole. */
        InterlockedExchange(&load->invalid, 1);
        DecodingDone(load);
        return;
    }
    load->decodeStarted = TRUE;
    if (!WorkersStart(&load->decodeJob, DecodeChunk, DecodingDone, load, load->chunkCount - 1)) {
        load->decodeStarted = FALSE;
        InterlockedExchange(&load->invalid, 1);
        DecodingDone(load);
    }
}

static void ReadRest(void *context, size_t index)
{
    TextLoad *load = context;
    UNREFERENCED_PARAMETER(index);
    if (!load->reader(load->context, load->available, load->size - load->available)) {
        load->error = GetLastError();
        if (load->error == ERROR_SUCCESS) {
            load->error = ERROR_READ_FAULT;
        }
    }
}

/* Runs on the worker that read the rest: the end of the data is known now, so the chunks can be made. */
static void ReadingDone(void *context)
{
    TextLoad *load = context;
    if (load->error != 0) {
        DecodingDone(load);
        return;
    }
    load->available = load->size;
    if (load->encoding == TEXT_ENCODING_UTF8) {
        size_t incomplete = TextTrailingIncompleteUtf8(load->data + load->bodyStart, load->size - load->bodyStart);
        load->bodyEnd = load->size - incomplete;
        load->trailingIncomplete = incomplete > 0;
    }
    size_t start = load->chunks[0].end;
    if (start >= load->bodyEnd || !AddChunks(load, start, PieceSize(load, load->bodyEnd - start))) {
        if (start < load->bodyEnd) {
            InterlockedExchange(&load->invalid, 1);
        }
        DecodingDone(load);
        return;
    }
    load->countStarted = TRUE;
    if (!WorkersStart(&load->countJob, CountChunk, CountingDone, load, load->chunkCount - 1)) {
        load->countStarted = FALSE;
        InterlockedExchange(&load->invalid, 1);
        DecodingDone(load);
    }
}

/* ---- Load ---------------------------------------------------------------------------------- */

static void DetectFormat(TextLoad *load)
{
    const unsigned char *data = load->data;
    size_t size = load->size;
    size_t available = load->available;
    TextFormat format = TextDefaultFormat();
    TextEncoding utf16 = TEXT_ENCODING_UTF16LE;
    load->bodyStart = 0;
    load->bodyEnd = size;
    load->encoding = TEXT_ENCODING_UTF8;

    if (available >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        format.byteOrderMark = TRUE;
        load->bodyStart = 3;
    } else if (available >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        format.encoding = load->encoding = TEXT_ENCODING_UTF16LE;
        format.byteOrderMark = TRUE;
        load->bodyStart = 2;
    } else if (available >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
        format.encoding = load->encoding = TEXT_ENCODING_UTF16BE;
        format.byteOrderMark = TRUE;
        load->bodyStart = 2;
    } else if (TextLooksLikeUtf16(data, available, &utf16)) {
        format.encoding = load->encoding = utf16;
    }

    if (load->encoding == TEXT_ENCODING_UTF8) {
        /* The end of the data decides this; it is looked at again once everything is read. */
        size_t incomplete = available == size ? TextTrailingIncompleteUtf8(data + load->bodyStart, size - load->bodyStart) : 0;
        load->bodyEnd = size - incomplete;
        load->trailingIncomplete = incomplete > 0;
    } else {
        size_t body = size - load->bodyStart;
        load->bodyEnd = load->bodyStart + (body & ~(size_t)1);
        load->trailingIncomplete = (body & 1) != 0;
    }
    *load->format = format;
}

/* Decodes the whole body with the ANSI code page into the load's own buffers. */
static BOOL DecodeAnsi(TextLoad *load)
{
    size_t size = load->size;
    int written = 0;
    if (size > 0) {
        if (size > 0x7FFFFFF0 || size > load->textCapacity - 2) {
            return FALSE;
        }
        written = MultiByteToWideChar(CP_ACP, 0, (const char *)load->data, (int)size, load->text, (int)(load->textCapacity - 2));
        if (written == 0) {
            return FALSE;
        }
    }
    load->length = NormalizeAndIndex(load->text, (size_t)written, load->lineStarts, &load->lineCount);
    load->format->encoding = TEXT_ENCODING_ANSI;
    load->format->byteOrderMark = FALSE;
    return TRUE;
}

/* The line ending of the data: the first break decides, and data without one gets CRLF. */
static LineEnding DetectLineEnding(const TextLoad *load)
{
    const unsigned char *data = load->data + load->bodyStart;
    size_t size = load->bodyEnd - load->bodyStart;
    if (load->encoding == TEXT_ENCODING_UTF8 || load->format->encoding == TEXT_ENCODING_ANSI) {
        for (size_t i = 0; i < size; ++i) {
            if (data[i] == '\n') {
                return LINE_ENDING_LF;
            }
            if (data[i] == '\r') {
                return i + 1 < size && data[i + 1] == '\n' ? LINE_ENDING_CRLF : LINE_ENDING_CR;
            }
        }
        return LINE_ENDING_CRLF;
    }
    BOOL bigEndian = load->encoding == TEXT_ENCODING_UTF16BE;
    for (size_t i = 0; i + 1 < size; i += 2) {
        unsigned unit = Unit16(data, i, bigEndian);
        if (unit == '\n') {
            return LINE_ENDING_LF;
        }
        if (unit == '\r') {
            return i + 3 < size && Unit16(data, i + 2, bigEndian) == '\n' ? LINE_ENDING_CRLF : LINE_ENDING_CR;
        }
    }
    return LINE_ENDING_CRLF;
}

/* Everything decoded: the trailing sequence, the ANSI fallback and the final counts. */
static void Finish(TextLoad *load)
{
    if (load->complete) {
        return;
    }
    if (load->error != 0) {
        /* Only the first part was read; the text ends where it ends. */
        load->length = load->chunks[0].written;
        load->lineCount = 1 + load->chunks[0].lines;
        load->bodyEnd = load->chunks[0].end;
        load->format->lineEnding = DetectLineEnding(load);
        load->complete = TRUE;
        return;
    }
    BOOL ansi = load->invalid != 0;
    if (!ansi) {
        size_t length = load->chunks[0].written;
        size_t lines = 1 + load->chunks[0].lines;
        for (size_t i = 1; i < load->chunkCount; ++i) {
            length += load->chunks[i].written;
            lines += load->chunks[i].lines;
        }
        load->anyNonAscii = load->anyNonAscii || load->chunks[0].nonAscii;
        if (load->trailingIncomplete) {
            if (load->encoding == TEXT_ENCODING_UTF8 && !load->anyNonAscii) {
                /* Only ASCII before a cut-off sequence is no evidence of UTF-8: an ANSI letter can look like a lead byte. */
                ansi = TRUE;
            } else {
                load->text[length++] = 0xFFFD;
            }
        }
        load->length = length;
        load->lineCount = lines;
    }
    if (ansi && !DecodeAnsi(load)) {
        load->length = 0;
        load->lineCount = 1;
        load->lineStarts[0] = 0;
    }
    load->format->lineEnding = DetectLineEnding(load);
    load->complete = TRUE;
}

TextLoad *TextLoadBegin(unsigned char *data, size_t available, size_t size, TextLoadReader *reader, TextLoadRelease *release,
    void *context, TextFormat *format, HWND notifyWindow, UINT notifyMessage)
{
    if (size > 0x7FFFFFF0 || available > size || (available < size && (reader == NULL || available < TEXTLOAD_FIRST_BYTES))) {
        return NULL;
    }
    TextLoad *load = MemAllocZero(sizeof *load);
    if (load == NULL) {
        return NULL;
    }
    load->data = data;
    load->size = size;
    load->available = available;
    load->reader = reader;
    load->release = release;
    load->context = context;
    load->format = format;
    load->notifyWindow = notifyWindow;
    load->notifyMessage = notifyMessage;
    load->workers = WorkersInitialize();
    DetectFormat(load);

    /* One unit per byte is the most UTF-8 and the ANSI code pages decode to; a break is at least a byte. */
    size_t unitBound = (load->encoding == TEXT_ENCODING_UTF8 ? size : size / 2) + 2;
    size_t textBytes = (unitBound + TEXT_SLACK) * sizeof(wchar_t);
    size_t textCapacityBytes = 0;
    load->text = BlockTake(textBytes, &textCapacityBytes);
    load->textCapacity = textCapacityBytes / sizeof(wchar_t);
    load->lineCapacity = unitBound;
    load->lineStarts = VirtualAlloc(NULL, load->lineCapacity * sizeof(size_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    load->chunkCapacity = 16;
    load->chunks = MemAllocZero(load->chunkCapacity * sizeof(TextLoadChunk));
    if (load->text == NULL || load->lineStarts == NULL || load->chunks == NULL) {
        load->release = NULL;
        TextLoadDiscard(load);
        return NULL;
    }
    load->lineStarts[0] = 0;

    /* The first chunk ends inside what is read, short of the end by the longest sequence when more is to come. */
    size_t limit = available == size ? load->bodyEnd : available - 4;
    size_t first = load->bodyStart + FIRST_CHUNK;
    first = Boundary(load, limit, first < limit ? first : limit);
    TextLoadChunk *chunk = &load->chunks[0];
    chunk->start = load->bodyStart;
    chunk->end = first;
    load->chunkCount = 1;

    Decoded decoded;
    if (load->encoding == TEXT_ENCODING_UTF8) {
        decoded = DecodeUtf8(data + chunk->start, chunk->end - chunk->start, load->text, load->textCapacity - 2, 0, load->lineStarts + 1);
        chunk->nonAscii = FALSE;
        for (size_t i = chunk->start; i < chunk->end && !chunk->nonAscii; ++i) {
            chunk->nonAscii = data[i] >= 0x80;
        }
    } else {
        decoded = DecodeUtf16(data + chunk->start, chunk->end - chunk->start, load->encoding == TEXT_ENCODING_UTF16BE,
            load->text, load->textCapacity - 2, 0, load->lineStarts + 1);
        chunk->nonAscii = TRUE;
    }
    chunk->written = decoded.written;
    chunk->lines = decoded.lines;
    chunk->units = decoded.written;
    chunk->breaks = decoded.lines;
    load->length = decoded.written;
    load->lineCount = 1 + decoded.lines;
    if (decoded.invalid) {
        load->invalid = 1;
    }

    if (load->invalid || (available == size && chunk->end >= load->bodyEnd)) {
        /* Nothing is left for the workers: an invalid first part ends as ANSI, a small file is done. */
        if (available < size) {
            load->started = TRUE;
            load->readStarted = TRUE;
            WorkersStart(&load->readJob, ReadRest, NULL, load, 1);
            WorkersWait(&load->readJob);
            load->readStarted = FALSE;
            load->available = load->error == 0 ? size : available;
        }
        Finish(load);
    }
    return load;
}

void TextLoadStart(TextLoad *load)
{
    if (load->started || load->complete) {
        return;
    }
    load->started = TRUE;
    if (load->available < load->size) {
        load->readStarted = TRUE;
        if (!WorkersStart(&load->readJob, ReadRest, ReadingDone, load, 1)) {
            load->readStarted = FALSE;
            load->error = ERROR_NOT_ENOUGH_MEMORY;
            DecodingDone(load);
        }
        return;
    }
    /* Everything is read already: straight to counting the rest. */
    ReadingDone(load);
}

BOOL TextLoadIsComplete(const TextLoad *load)
{
    return load->complete || load->decoded != 0;
}

void TextLoadWait(TextLoad *load)
{
    if (load->complete) {
        return;
    }
    TextLoadStart(load);
    if (load->readStarted) {
        WorkersWait(&load->readJob);
        load->readStarted = FALSE;
    }
    if (load->countStarted) {
        WorkersWait(&load->countJob);
        load->countStarted = FALSE;
    }
    if (load->decodeStarted) {
        WorkersWait(&load->decodeJob);
        load->decodeStarted = FALSE;
    }
    Finish(load);
}

void TextLoadEnd(TextLoad *load)
{
    TextLoadWait(load);
    if (load->release != NULL) {
        load->release(load->context);
    }
    MemFree(load->chunks);
    MemFree(load);
}

void TextLoadDiscard(TextLoad *load)
{
    if (load->started && !load->complete) {
        TextLoadWait(load);
    }
    if (load->release != NULL) {
        load->release(load->context);
    }
    BlockReturn(load->text, load->textCapacity * sizeof(wchar_t));
    if (load->lineStarts != NULL) {
        VirtualFree(load->lineStarts, 0, MEM_RELEASE);
    }
    MemFree(load->chunks);
    MemFree(load);
}
