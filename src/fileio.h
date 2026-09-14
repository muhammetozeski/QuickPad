#pragma once

#include <windows.h>

#include "text.h"
#include "textload.h"

/*
 * Reads and decodes a whole file. The text is a MemAlloc block. Files other programs are
 * writing to are still read. On failure *error receives a Win32 error code.
 */
BOOL FileLoad(const wchar_t *path, wchar_t **text, size_t *length, TextFormat *format, DWORD *error);

/*
 * Opens a file and starts decoding it; see TextLoadBegin for the load, its format and the notify
 * window. The first part is read here, the rest by a worker once the load is started.
 * On failure *error receives a Win32 error code.
 */
BOOL FileBeginLoad(const wchar_t *path, TextFormat *format, HWND notifyWindow, UINT notifyMessage, TextLoad **load, DWORD *error);

/*
 * Writes data to a temporary file next to path and swaps it in with ReplaceFileW, which keeps the
 * original's attributes and permissions. Links, folders that do not allow new files and failed
 * swaps fall back to writing the file in place.
 */
BOOL FileWrite(const wchar_t *path, const unsigned char *data, size_t size, DWORD *error);
