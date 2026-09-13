#pragma once

#include <windows.h>

#include "text.h"

/*
 * Reads and decodes a whole file. The text is a MemAlloc block. Files other programs are
 * writing to are still read. On failure *error receives a Win32 error code.
 */
BOOL FileLoad(const wchar_t *path, wchar_t **text, size_t *length, TextFormat *format, DWORD *error);
