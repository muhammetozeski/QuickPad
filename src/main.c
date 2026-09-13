#include "editor.h"
#include "quickpad.h"

static const wchar_t *SkipProgramName(const wchar_t *cursor)
{
    if (*cursor == L'"') {
        ++cursor;
        while (*cursor != 0 && *cursor != L'"') {
            ++cursor;
        }
        if (*cursor == L'"') {
            ++cursor;
        }
    } else {
        while (*cursor != 0 && *cursor != L' ' && *cursor != L'\t') {
            ++cursor;
        }
    }
    return cursor;
}

/* The next argument in a MemAlloc block, split and unquoted the way CommandLineToArgvW does it; NULL at the end. */
static wchar_t *NextArgument(const wchar_t **cursor)
{
    const wchar_t *p = *cursor;
    while (*p == L' ' || *p == L'\t') {
        ++p;
    }
    if (*p == 0) {
        *cursor = p;
        return NULL;
    }

    wchar_t *argument = MemAlloc(((size_t)lstrlenW(p) + 1) * sizeof(wchar_t));
    if (argument == NULL) {
        *cursor = p + lstrlenW(p);
        return NULL;
    }

    size_t length = 0;
    BOOL quoted = FALSE;
    while (*p != 0 && (quoted || (*p != L' ' && *p != L'\t'))) {
        if (*p == L'\\') {
            size_t backslashes = 0;
            while (*p == L'\\') {
                ++backslashes;
                ++p;
            }
            if (*p == L'"') {
                for (size_t i = 0; i < backslashes / 2; ++i) {
                    argument[length++] = L'\\';
                }
                if (backslashes % 2 == 1) {
                    argument[length++] = L'"';
                    ++p;
                }
            } else {
                for (size_t i = 0; i < backslashes; ++i) {
                    argument[length++] = L'\\';
                }
            }
        } else if (*p == L'"') {
            if (quoted && p[1] == L'"') {
                argument[length++] = L'"';
                p += 2;
            } else {
                quoted = !quoted;
                ++p;
            }
        } else {
            argument[length++] = *p++;
        }
    }
    argument[length] = 0;
    *cursor = p;
    return argument;
}

static wchar_t *FullPath(const wchar_t *path)
{
    DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    wchar_t *full = needed != 0 ? MemAlloc(needed * sizeof(wchar_t)) : NULL;
    if (full != NULL && GetFullPathNameW(path, needed, full, NULL) == 0) {
        MemFree(full);
        full = NULL;
    }
    return full;
}

static int QuickPadMain(void)
{
    if (!EditorInitialize(GetModuleHandleW(NULL))) {
        return 1;
    }

    BOOL opened = FALSE;
    const wchar_t *cursor = SkipProgramName(GetCommandLineW());
    for (wchar_t *argument = NextArgument(&cursor); argument != NULL; argument = NextArgument(&cursor)) {
        wchar_t *path = FullPath(argument);
        if (path != NULL) {
            opened |= EditorOpenFile(path);
            MemFree(path);
        }
        MemFree(argument);
    }
    if (!opened && !EditorOpenNew()) {
        return 1;
    }

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (!EditorTranslateMessage(&message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return (int)message.wParam;
}

__declspec(noreturn) void QuickPadEntry(void)
{
    ExitProcess((UINT)QuickPadMain());
}
