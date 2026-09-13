#include "document.h"
#include "quickpad.h"

#include <emmintrin.h>
#include <intrin.h>

#define GAP_MINIMUM 4096
#define LINE_RESERVE 256

static size_t GapSize(const Document *document)
{
    return document->gapEnd - document->gapStart;
}

/* Index of the first ch at or after from, or length. */
static size_t FindCharacter(const wchar_t *text, size_t length, wchar_t ch, size_t from)
{
    const __m128i pattern = _mm_set1_epi16((short)ch);
    size_t i = from;
    for (; i + 8 <= length; i += 8) {
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi16(_mm_loadu_si128((const __m128i *)(text + i)), pattern));
        if (mask != 0) {
            unsigned long bit;
            _BitScanForward(&bit, (unsigned long)mask);
            return i + bit / 2;
        }
    }
    for (; i < length; ++i) {
        if (text[i] == ch) {
            return i;
        }
    }
    return length;
}

static unsigned CountBits16(unsigned value)
{
    value = value - ((value >> 1) & 0x5555);
    value = (value & 0x3333) + ((value >> 2) & 0x3333);
    value = (value + (value >> 4)) & 0x0F0F;
    return (value + (value >> 8)) & 0x1F;
}

static size_t CountLineBreaks(const wchar_t *text, size_t length)
{
    const __m128i lf = _mm_set1_epi16(L'\n');
    size_t count = 0;
    size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi16(_mm_loadu_si128((const __m128i *)(text + i)), lf));
        count += CountBits16((unsigned)mask & 0x5555);
    }
    for (; i < length; ++i) {
        count += text[i] == L'\n';
    }
    return count;
}

size_t DocumentNormalizeLineBreaks(wchar_t *text, size_t length)
{
    size_t read = FindCharacter(text, length, L'\r', 0);
    size_t write = read;
    while (read < length) {
        /* text[read] is a CR here. */
        ++read;
        text[write++] = L'\n';
        if (read < length && text[read] == L'\n') {
            ++read;
        }

        size_t next = FindCharacter(text, length, L'\r', read);
        size_t segment = next - read;
        if (segment > 0) {
            memmove(text + write, text + read, segment * sizeof(wchar_t));
            write += segment;
        }
        read = next;
    }
    return write;
}

BOOL DocumentInitialize(Document *document)
{
    Document empty = { 0 };
    empty.capacity = GAP_MINIMUM;
    empty.gapEnd = GAP_MINIMUM;
    empty.lineCapacity = LINE_RESERVE;
    empty.lineCount = 1;
    empty.buffer = MemAlloc(empty.capacity * sizeof(wchar_t));
    empty.lineStarts = MemAlloc(empty.lineCapacity * sizeof(size_t));
    if (empty.buffer == NULL || empty.lineStarts == NULL) {
        MemFree(empty.buffer);
        MemFree(empty.lineStarts);
        return FALSE;
    }
    empty.lineStarts[0] = 0;
    *document = empty;
    return TRUE;
}

void DocumentRelease(Document *document)
{
    MemFree(document->buffer);
    MemFree(document->lineStarts);
    Document empty = { 0 };
    *document = empty;
}

BOOL DocumentAdopt(Document *document, wchar_t *text, size_t length)
{
    size_t capacity = HeapSize(GetProcessHeap(), 0, text) / sizeof(wchar_t);
    length = DocumentNormalizeLineBreaks(text, length);

    size_t breaks = CountLineBreaks(text, length);
    size_t lineCapacity = breaks + 1 + LINE_RESERVE;
    size_t *starts = MemAlloc(lineCapacity * sizeof(size_t));
    if (starts == NULL) {
        return FALSE;
    }

    const __m128i lf = _mm_set1_epi16(L'\n');
    size_t line = 0;
    starts[line++] = 0;
    size_t i = 0;
    for (; i + 8 <= length; i += 8) {
        unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi16(_mm_loadu_si128((const __m128i *)(text + i)), lf)) & 0x5555;
        while (mask != 0) {
            unsigned long bit;
            _BitScanForward(&bit, mask);
            starts[line++] = i + bit / 2 + 1;
            mask &= mask - 1;
        }
    }
    for (; i < length; ++i) {
        if (text[i] == L'\n') {
            starts[line++] = i + 1;
        }
    }

    MemFree(document->buffer);
    MemFree(document->lineStarts);
    document->buffer = text;
    document->capacity = capacity;
    document->gapStart = length;
    document->gapEnd = capacity;
    document->lineStarts = starts;
    document->lineCount = line;
    document->lineCapacity = lineCapacity;
    return TRUE;
}

size_t DocumentLength(const Document *document)
{
    return document->capacity - GapSize(document);
}

wchar_t DocumentCharAt(const Document *document, size_t position)
{
    return position < document->gapStart ? document->buffer[position] : document->buffer[position + GapSize(document)];
}

void DocumentCopy(const Document *document, size_t start, size_t count, wchar_t *destination)
{
    if (start < document->gapStart) {
        size_t before = document->gapStart - start;
        if (before > count) {
            before = count;
        }
        memcpy(destination, document->buffer + start, before * sizeof(wchar_t));
        destination += before;
        start += before;
        count -= before;
    }
    if (count > 0) {
        memcpy(destination, document->buffer + start + GapSize(document), count * sizeof(wchar_t));
    }
}

const wchar_t *DocumentPeek(const Document *document, size_t start, size_t count)
{
    if (start + count <= document->gapStart) {
        return document->buffer + start;
    }
    if (start >= document->gapStart) {
        return document->buffer + start + GapSize(document);
    }
    return NULL;
}

static void MoveGap(Document *document, size_t position)
{
    size_t gap = GapSize(document);
    if (position < document->gapStart) {
        memmove(document->buffer + position + gap, document->buffer + position,
            (document->gapStart - position) * sizeof(wchar_t));
    } else if (position > document->gapStart) {
        memmove(document->buffer + document->gapStart, document->buffer + document->gapEnd,
            (position - document->gapStart) * sizeof(wchar_t));
    }
    document->gapStart = position;
    document->gapEnd = position + gap;
}

static BOOL EnsureGap(Document *document, size_t needed)
{
    if (GapSize(document) >= needed) {
        return TRUE;
    }

    size_t length = DocumentLength(document);
    size_t capacity = length + needed + GAP_MINIMUM + length / 8;
    wchar_t *buffer = MemAlloc(capacity * sizeof(wchar_t));
    if (buffer == NULL) {
        return FALSE;
    }

    size_t after = document->capacity - document->gapEnd;
    memcpy(buffer, document->buffer, document->gapStart * sizeof(wchar_t));
    memcpy(buffer + capacity - after, document->buffer + document->gapEnd, after * sizeof(wchar_t));
    MemFree(document->buffer);
    document->buffer = buffer;
    document->gapEnd = capacity - after;
    document->capacity = capacity;
    return TRUE;
}

static BOOL EnsureLineCapacity(Document *document, size_t lines)
{
    if (lines <= document->lineCapacity) {
        return TRUE;
    }

    size_t capacity = lines + LINE_RESERVE + lines / 8;
    size_t *starts = MemAlloc(capacity * sizeof(size_t));
    if (starts == NULL) {
        return FALSE;
    }
    memcpy(starts, document->lineStarts, document->lineCount * sizeof(size_t));
    MemFree(document->lineStarts);
    document->lineStarts = starts;
    document->lineCapacity = capacity;
    return TRUE;
}

const wchar_t *DocumentText(Document *document)
{
    if (!EnsureGap(document, 1)) {
        return NULL;
    }
    size_t length = DocumentLength(document);
    MoveGap(document, length);
    document->buffer[length] = 0;
    return document->buffer;
}

BOOL DocumentInsert(Document *document, size_t position, const wchar_t *text, size_t count)
{
    if (count == 0) {
        return TRUE;
    }

    size_t breaks = CountLineBreaks(text, count);
    if (!EnsureLineCapacity(document, document->lineCount + breaks) || !EnsureGap(document, count)) {
        return FALSE;
    }

    size_t line = DocumentLineFromPosition(document, position);
    MoveGap(document, position);
    memcpy(document->buffer + position, text, count * sizeof(wchar_t));
    document->gapStart += count;

    size_t *starts = document->lineStarts;
    size_t following = document->lineCount - (line + 1);
    if (breaks > 0 && following > 0) {
        memmove(starts + line + 1 + breaks, starts + line + 1, following * sizeof(size_t));
    }
    for (size_t i = line + 1 + breaks; i < document->lineCount + breaks; ++i) {
        starts[i] += count;
    }
    if (breaks > 0) {
        size_t next = line + 1;
        for (size_t i = FindCharacter(text, count, L'\n', 0); i < count; i = FindCharacter(text, count, L'\n', i + 1)) {
            starts[next++] = position + i + 1;
        }
    }
    document->lineCount += breaks;
    return TRUE;
}

void DocumentDelete(Document *document, size_t start, size_t count)
{
    if (count == 0) {
        return;
    }

    size_t firstLine = DocumentLineFromPosition(document, start);
    size_t lastLine = DocumentLineFromPosition(document, start + count);
    MoveGap(document, start);
    document->gapEnd += count;

    size_t removed = lastLine - firstLine;
    size_t *starts = document->lineStarts;
    size_t following = document->lineCount - (lastLine + 1);
    if (removed > 0 && following > 0) {
        memmove(starts + firstLine + 1, starts + lastLine + 1, following * sizeof(size_t));
    }
    document->lineCount -= removed;
    for (size_t i = firstLine + 1; i < document->lineCount; ++i) {
        starts[i] -= count;
    }
}

size_t DocumentLineCount(const Document *document)
{
    return document->lineCount;
}

size_t DocumentLineStart(const Document *document, size_t line)
{
    return document->lineStarts[line];
}

size_t DocumentLineEnd(const Document *document, size_t line)
{
    return line + 1 < document->lineCount ? document->lineStarts[line + 1] - 1 : DocumentLength(document);
}

size_t DocumentLineFromPosition(const Document *document, size_t position)
{
    size_t low = 0;
    size_t high = document->lineCount;
    while (high - low > 1) {
        size_t middle = low + (high - low) / 2;
        if (document->lineStarts[middle] <= position) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}
