#pragma once

#include <windows.h>

#include "textload.h"

/*
 * The text of one editor: a gap buffer of UTF-16 characters plus the start offset of every line.
 * Line breaks are stored as L'\n' only; the file's own line ending is applied again when saving.
 * Positions are character offsets in the text, not counting the gap.
 *
 * While a file is still being decoded (load is set) the document holds what has been decoded so
 * far, with its last line possibly cut short. Reading that part is fine; anything else calls
 * DocumentFinishLoad first, which waits for the rest.
 */
typedef struct Document {
    wchar_t *buffer;
    size_t capacity;
    size_t gapStart;
    size_t gapEnd;
    size_t *lineStarts;
    size_t lineCount;
    size_t lineCapacity;
    BOOL bufferIsBlock;    /* buffer comes from BlockTake, otherwise from MemAlloc */
    BOOL indexIsBlock;     /* lineStarts comes from VirtualAlloc, otherwise from MemAlloc */
    TextLoad *load;        /* the load still decoding into buffer, or NULL */
} Document;

BOOL DocumentInitialize(Document *document);
void DocumentRelease(Document *document);

/*
 * Replaces the content with text, a block allocated with MemAlloc that the document takes over
 * on success. CRLF and CR are converted to LF in place first.
 */
BOOL DocumentAdopt(Document *document, wchar_t *text, size_t length);

/* Replaces the content with a load from TextLoadBegin, which the document takes over. */
void DocumentAdoptLoad(Document *document, TextLoad *load);

/* Hands the rest of the load to the worker threads; see TextLoadStart. */
void DocumentStartLoad(Document *document);

/* Waits for the load still decoding, if any, and makes the text final; TRUE when there was one. */
BOOL DocumentFinishLoad(Document *document);

/* Converts CRLF and CR to LF in place and returns the new length. */
size_t DocumentNormalizeLineBreaks(wchar_t *text, size_t length);

size_t DocumentLength(const Document *document);
wchar_t DocumentCharAt(const Document *document, size_t position);
void DocumentCopy(const Document *document, size_t start, size_t count, wchar_t *destination);

/* Pointer to text[start, start + count) when the range does not cross the gap, otherwise NULL. */
const wchar_t *DocumentPeek(const Document *document, size_t start, size_t count);

/* Moves the gap to the end and returns the whole text, null-terminated. */
const wchar_t *DocumentText(Document *document);

/* text must use L'\n' line breaks. */
BOOL DocumentInsert(Document *document, size_t position, const wchar_t *text, size_t count);
void DocumentDelete(Document *document, size_t start, size_t count);

size_t DocumentLineCount(const Document *document);
size_t DocumentLineStart(const Document *document, size_t line);
/* Position of the line's L'\n', or the text length for the last line. */
size_t DocumentLineEnd(const Document *document, size_t line);
size_t DocumentLineFromPosition(const Document *document, size_t position);
