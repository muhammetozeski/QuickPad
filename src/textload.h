#pragma once

#include <windows.h>

#include "text.h"
#include "workers.h"

/*
 * Decodes file contents into document text without making the caller wait for all of it.
 *
 * TextLoadBegin detects the encoding, decodes the first part of the data on the calling thread and
 * returns. Once TextLoadStart is called (or TextLoadWait is), the worker threads read the rest of
 * the data, count it and decode it. The text and line index are usable at once for what has been
 * decoded: text[0, length) and lineStarts[0, lineCount), where the last line may be cut short
 * until the load is complete. TextLoadWait finishes the load and makes them final. The caller keeps
 * text (a BlockTake block) and lineStarts (a VirtualAlloc block) after TextLoadEnd.
 *
 * The decoded text has L'\n' line breaks only and no NUL characters. Data that is not valid UTF-8
 * is decoded with the ANSI code page, which can only be known once every part has been looked at;
 * the format is then updated when the load is waited for.
 */

/* Bytes the caller must have read before TextLoadBegin, unless the data is shorter. */
#define TEXTLOAD_FIRST_BYTES (64 * 1024 + 4096)

/* Reads data[offset, offset + length) on a worker thread; FALSE with the last error set on failure. */
typedef BOOL TextLoadReader(void *context, size_t offset, size_t length);
typedef void TextLoadRelease(void *context);

typedef struct TextLoadChunk {
    size_t start;        /* byte range in the data */
    size_t end;
    size_t units;        /* from counting: UTF-16 units and line breaks the chunk decodes to */
    size_t breaks;
    BOOL nonAscii;
    size_t outOffset;    /* from the counts of the chunks before it */
    size_t lineOffset;
    size_t written;      /* from decoding */
    size_t lines;
} TextLoadChunk;

typedef struct TextLoad {
    unsigned char *data;
    size_t size;
    size_t available;           /* bytes of data read so far */
    TextLoadReader *reader;
    TextLoadRelease *release;
    void *context;
    TextFormat *format;         /* the caller's format, set at the start and again if the load ends as ANSI */
    TextEncoding encoding;
    size_t bodyStart;           /* after the byte order mark */
    size_t bodyEnd;             /* before a trailing incomplete sequence or odd byte */
    BOOL trailingIncomplete;

    wchar_t *text;
    size_t textCapacity;        /* units */
    size_t *lineStarts;
    size_t lineCapacity;        /* entries */
    size_t length;              /* units decoded so far, final once complete */
    size_t lineCount;           /* lines so far, counting a cut short last line, final once complete */
    BOOL complete;
    DWORD error;                /* a read error; the text then holds only the first part */

    TextLoadChunk *chunks;
    size_t chunkCount;
    size_t chunkCapacity;
    int workers;
    BOOL started;
    BOOL readStarted;
    BOOL countStarted;
    BOOL decodeStarted;
    volatile LONG decoded;      /* set by the worker that finished the last step */
    volatile LONG invalid;
    BOOL anyNonAscii;
    WorkerJob readJob;
    WorkerJob countJob;
    WorkerJob decodeJob;
    HWND notifyWindow;
    UINT notifyMessage;
} TextLoad;

/*
 * Starts loading data, of which the first available bytes are read; reader fills the rest when
 * asked, and data stays valid until release is called with context. *format receives the detected
 * format. notifyWindow, when not NULL, is posted notifyMessage once every part is decoded.
 * Returns NULL when memory runs out, without calling release.
 */
TextLoad *TextLoadBegin(unsigned char *data, size_t available, size_t size, TextLoadReader *reader, TextLoadRelease *release,
    void *context, TextFormat *format, HWND notifyWindow, UINT notifyMessage);

/* Hands the rest of the work to the worker threads; nothing happens in the background before this. */
void TextLoadStart(TextLoad *load);

/* TRUE once every part is decoded, without waiting. */
BOOL TextLoadIsComplete(const TextLoad *load);

/* Waits for the workers, applies the ANSI fallback when needed and makes length and lineCount final. */
void TextLoadWait(TextLoad *load);

/* Releases the data and the load; the caller keeps text and lineStarts. Waits first if needed. */
void TextLoadEnd(TextLoad *load);

/* Releases the data and frees the load with its text and lineStarts, for a load the caller does not keep. */
void TextLoadDiscard(TextLoad *load);
