#include "strings.h"
#include "quickpad.h"

wchar_t *StringCopy(const wchar_t *text)
{
    size_t size = ((size_t)lstrlenW(text) + 1) * sizeof(wchar_t);
    wchar_t *copy = MemAlloc(size);
    if (copy != NULL) {
        memcpy(copy, text, size);
    }
    return copy;
}

wchar_t *StringJoin(const wchar_t *first, const wchar_t *second, const wchar_t *third)
{
    size_t a = (size_t)lstrlenW(first);
    size_t b = (size_t)lstrlenW(second);
    size_t c = (size_t)lstrlenW(third);
    wchar_t *joined = MemAlloc((a + b + c + 1) * sizeof(wchar_t));
    if (joined != NULL) {
        memcpy(joined, first, a * sizeof(wchar_t));
        memcpy(joined + a, second, b * sizeof(wchar_t));
        memcpy(joined + a + b, third, (c + 1) * sizeof(wchar_t));
    }
    return joined;
}

const wchar_t *PathFileName(const wchar_t *path)
{
    const wchar_t *name = path;
    for (const wchar_t *p = path; *p != 0; ++p) {
        if (*p == L'\\' || *p == L'/') {
            name = p + 1;
        }
    }
    return name;
}

wchar_t *PathOfExecutable(void)
{
    for (DWORD capacity = MAX_PATH; capacity <= 32768; capacity *= 2) {
        wchar_t *path = MemAlloc(capacity * sizeof(wchar_t));
        if (path == NULL) {
            return NULL;
        }
        DWORD length = GetModuleFileNameW(NULL, path, capacity);
        if (length > 0 && length < capacity) {
            return path;
        }
        MemFree(path);
        if (length == 0) {
            return NULL;
        }
    }
    return NULL;
}
