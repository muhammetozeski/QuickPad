#include "document.h"
#include "blocks.h"
#include "quickpad.h"

#include <emmintrin.h>
#include <intrin.h>

#define GAP_MINIMUM 4096
#define LINE_RESERVE 256

/* Text of this many units and up lives in a ready block; a line index of this many entries and up in its own pages. */
#define BLOCK_UNITS (BLOCK_MINIMUM / 2 / sizeof(wchar_t))
#define INDEX_BLOCK_ENTRIES (64 * 1024)
#define PAGE 4096

static size_t GapSize(const Document *document)
{
    return document->gapEnd - document->gapStart;
}

static wchar_t *AllocText(size_t units, size_t *capacity, BOOL *isBlock)
{
    if (units >= BLOCK_UNITS) {
        size_t bytes = 0;
        wchar_t *text = BlockTake(units * sizeof(wchar_t), &bytes);
        *capacity = bytes / sizeof(wchar_t);
        *isBlock = TRUE;
        return text;
    }
    *capacity = units;
    *isBlock = FALSE;
    return MemAlloc(units * sizeof(wchar_t));
}

static void FreeText(wchar_t *text, size_t capacity, BOOL isBlock)
{
    if (isBlock) {
        BlockReturn(text, capacity * sizeof(wchar_t));
    } else {
        MemFree(text);
    }
}

static size_t *AllocIndex(size_t entries, size_t *capacity, BOOL *isBlock)
{
    if (entries >= INDEX_BLOCK_ENTRIES) {
        size_t bytes = (entries * sizeof(size_t) + PAGE - 1) & ~(size_t)(PAGE - 1);
        size_t *starts = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        *capacity = bytes / sizeof(size_t);
        *isBlock = TRUE;
        return starts;
    }
    *capacity = entries;
    *isBlock = FALSE;
    return MemAlloc(entries * sizeof(size_t));
}

static void FreeIndex(size_t *starts, BOOL isBlock)
{
    if (starts == NULL) {
        return;
    }
    if (isBlock) {
        VirtualFree(starts, 0, MEM_RELEASE);
    } else {
        MemFree(starts);
    }
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
    empty.bufferIsBlock = FALSE;
    empty.indexIsBlock = FALSE;
    empty.lineStarts[0] = 0;
    *document = empty;
    return TRUE;
}

void DocumentRelease(Document *document)
{
    DocumentFinishLoad(document);
    FreeText(document->buffer, document->capacity, document->bufferIsBlock);
    FreeIndex(document->lineStarts, document->indexIsBlock);
    Document empty = { 0 };
    *document = empty;
}

void DocumentAdoptLoad(Document *document, TextLoad *load)
{
    DocumentRelease(document);
    document->buffer = load->text;
    document->capacity = load->textCapacity;
    document->bufferIsBlock = TRUE;
    document->gapStart = load->length;
    document->gapEnd = load->textCapacity;
    document->lineStarts = load->lineStarts;
    document->lineCapacity = load->lineCapacity;
    document->indexIsBlock = TRUE;
    document->lineCount = load->lineCount;
    document->load = load;
    if (TextLoadIsComplete(load)) {
        DocumentFinishLoad(document);
    }
}

void DocumentStartLoad(Document *document)
{
    if (document->load != NULL) {
        TextLoadStart(document->load);
    }
}

BOOL DocumentFinishLoad(Document *document)
{
    TextLoad *load = document->load;
    if (load == NULL) {
        return FALSE;
    }
    TextLoadWait(load);
    document->gapStart = load->length;
    document->lineCount = load->lineCount;
    document->load = NULL;
    TextLoadEnd(load);

    /* The index was sized for a line per byte; the pages beyond the lines it got are given back. */
    size_t keep = ((document->lineCount + LINE_RESERVE) * sizeof(size_t) + PAGE - 1) & ~(size_t)(PAGE - 1);
    size_t have = document->lineCapacity * sizeof(size_t);
    if (document->indexIsBlock && have > keep && have - keep >= BLOCK_MINIMUM) {
        VirtualFree((unsigned char *)document->lineStarts + keep, have - keep, MEM_DECOMMIT);
        document->lineCapacity = keep / sizeof(size_t);
    }
    return TRUE;
}

BOOL DocumentAdopt(Document *document, wchar_t *text, size_t length)
{
    size_t capacity = HeapSize(GetProcessHeap(), 0, text) / sizeof(wchar_t);
    length = DocumentNormalizeLineBreaks(text, length);

    size_t breaks = CountLineBreaks(text, length);
    size_t lineCapacity = 0;
    BOOL indexIsBlock = FALSE;
    size_t *starts = AllocIndex(breaks + 1 + LINE_RESERVE, &lineCapacity, &indexIsBlock);
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

    DocumentRelease(document);
    document->buffer = text;
    document->capacity = capacity;
    document->bufferIsBlock = FALSE;
    document->gapStart = length;
    document->gapEnd = capacity;
    document->lineStarts = starts;
    document->lineCount = line;
    document->lineCapacity = lineCapacity;
    document->indexIsBlock = indexIsBlock;
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
    size_t capacity = 0;
    BOOL isBlock = FALSE;
    wchar_t *buffer = AllocText(length + needed + GAP_MINIMUM + length / 8, &capacity, &isBlock);
    if (buffer == NULL) {
        return FALSE;
    }

    size_t after = document->capacity - document->gapEnd;
    memcpy(buffer, document->buffer, document->gapStart * sizeof(wchar_t));
    memcpy(buffer + capacity - after, document->buffer + document->gapEnd, after * sizeof(wchar_t));
    FreeText(document->buffer, document->capacity, document->bufferIsBlock);
    document->buffer = buffer;
    document->bufferIsBlock = isBlock;
    document->gapEnd = capacity - after;
    document->capacity = capacity;
    return TRUE;
}

static BOOL EnsureLineCapacity(Document *document, size_t lines)
{
    if (lines <= document->lineCapacity) {
        return TRUE;
    }

    size_t capacity = 0;
    BOOL isBlock = FALSE;
    size_t *starts = AllocIndex(lines + LINE_RESERVE + lines / 8, &capacity, &isBlock);
    if (starts == NULL) {
        return FALSE;
    }
    memcpy(starts, document->lineStarts, document->lineCount * sizeof(size_t));
    FreeIndex(document->lineStarts, document->indexIsBlock);
    document->lineStarts = starts;
    document->lineCapacity = capacity;
    document->indexIsBlock = isBlock;
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
