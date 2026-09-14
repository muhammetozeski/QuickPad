#pragma once

#include <windows.h>

typedef enum TextEncoding {
    TEXT_ENCODING_UTF8,
    TEXT_ENCODING_UTF16LE,
    TEXT_ENCODING_UTF16BE,
    TEXT_ENCODING_ANSI,
} TextEncoding;

typedef enum LineEnding {
    LINE_ENDING_CRLF,
    LINE_ENDING_LF,
    LINE_ENDING_CR,
} LineEnding;

typedef struct TextFormat {
    TextEncoding encoding;
    BOOL byteOrderMark;
    LineEnding lineEnding;
} TextFormat;

/* The format of new documents: UTF-8 without a byte order mark and CRLF line endings. */
TextFormat TextDefaultFormat(void);

/* Text without a byte order mark counts as UTF-16 when most units of a sample have a zero byte on the same side. */
BOOL TextLooksLikeUtf16(const unsigned char *data, size_t size, TextEncoding *encoding);

/* Number of bytes at the end that start a UTF-8 sequence the data does not complete. */
size_t TextTrailingIncompleteUtf8(const unsigned char *data, size_t size);

/*
 * Detects the encoding and line ending of raw file contents and decodes them to UTF-16.
 * Line breaks are kept as they are in the file; NUL characters become spaces.
 * Returns a null-terminated buffer to release with MemFree, or NULL when memory runs out
 * or the contents are larger than the Win32 conversion functions accept.
 */
wchar_t *TextDecode(const unsigned char *data, size_t size, TextFormat *format, size_t *length);

/*
 * Encodes UTF-16 text in the given format. Every CR, LF and CRLF in the text is written as
 * the format's line ending. *lossy is set when a character has no ANSI representation.
 * Returns a buffer to release with MemFree, or NULL when memory runs out.
 */
unsigned char *TextEncode(const wchar_t *text, size_t length, TextFormat format, size_t *size, BOOL *lossy);
