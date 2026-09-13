#pragma once

#include <windows.h>

/* A MemAlloc copy of text, or NULL when memory runs out. */
wchar_t *StringCopy(const wchar_t *text);

/* The three strings joined in a MemAlloc block, or NULL when memory runs out. */
wchar_t *StringJoin(const wchar_t *first, const wchar_t *second, const wchar_t *third);

/* The part of a path after its last backslash or slash. */
const wchar_t *PathFileName(const wchar_t *path);

/* The full path of this executable in a MemAlloc block, or NULL. */
wchar_t *PathOfExecutable(void);
