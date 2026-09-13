#include "host.h"
#include "quickpad.h"
#include "register.h"

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

static BOOL SendToHost(HWND host, ULONG_PTR kind, const wchar_t *path)
{
    COPYDATASTRUCT data = { kind, 0, NULL };
    if (path != NULL) {
        data.cbData = (DWORD)(((size_t)lstrlenW(path) + 1) * sizeof(wchar_t));
        data.lpData = (PVOID)path;
    }
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(host, WM_COPYDATA, 0, (LPARAM)&data, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result) != 0
        && result == TRUE;
}

/* Hands the launch to the running host. Returns how many paths it accepted, or count + 1 for a launch without paths. */
static size_t Forward(HWND host, BOOL background, wchar_t **paths, size_t count)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(host, &processId);
    AllowSetForegroundWindow(processId);
    if (count == 0) {
        return background || SendToHost(host, HOST_COPY_NEW, NULL) ? 1 : 0;
    }
    size_t sent = 0;
    while (sent < count && SendToHost(host, HOST_COPY_OPEN, paths[sent])) {
        ++sent;
    }
    return sent;
}

/* TRUE when the whole launch went to the host; otherwise paths and count describe what is left. */
static BOOL ForwardAll(HWND host, BOOL background, wchar_t ***paths, size_t *count)
{
    size_t sent = Forward(host, background, *paths, *count);
    if (*count == 0) {
        return sent == 1;
    }
    *paths += sent;
    *count -= sent;
    return *count == 0;
}

static int QuickPadMain(void)
{
    const wchar_t *commandLine = SkipProgramName(GetCommandLineW());
    wchar_t **paths = MemAlloc(((size_t)lstrlenW(commandLine) / 2 + 1) * sizeof(wchar_t *));
    if (paths == NULL) {
        return 1;
    }
    size_t count = 0;
    BOOL background = FALSE;
    for (wchar_t *argument = NextArgument(&commandLine); argument != NULL; argument = NextArgument(&commandLine)) {
        if (CompareStringOrdinal(argument, -1, L"--register", -1, TRUE) == CSTR_EQUAL) {
            return RegisterFileTypes();
        }
        if (CompareStringOrdinal(argument, -1, L"--background", -1, TRUE) == CSTR_EQUAL) {
            background = TRUE;
        } else {
            wchar_t *path = FullPath(argument);
            if (path != NULL) {
                paths[count++] = path;
            }
        }
        MemFree(argument);
    }

    HWND host = FindWindowW(HOST_WINDOW_CLASS, NULL);
    if (host != NULL && ForwardAll(host, background, &paths, &count)) {
        return 0;
    }

    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, HOST_READY_EVENT_NAME);
    HANDLE mutex = CreateMutexW(NULL, FALSE, HOST_MUTEX_NAME);
    if (host != NULL || ready == NULL || mutex == NULL) {
        return HostRun(FALSE, NULL, background, paths, count);
    }

    for (;;) {
        host = FindWindowW(HOST_WINDOW_CLASS, NULL);
        if (host != NULL) {
            return ForwardAll(host, background, &paths, &count) ? 0 : HostRun(FALSE, NULL, background, paths, count);
        }

        /* No host window: another launch is becoming the host (the event), or nobody is (the mutex). */
        HANDLE handles[2] = { ready, mutex };
        DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            if (FindWindowW(HOST_WINDOW_CLASS, NULL) != NULL) {
                continue;
            }
            /* The event was left set by a host that ended abnormally; take over once its mutex is free. */
            DWORD owned = WaitForSingleObject(mutex, INFINITE);
            wait = owned == WAIT_OBJECT_0 || owned == WAIT_ABANDONED ? WAIT_OBJECT_0 + 1 : WAIT_FAILED;
        }
        if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_ABANDONED_0 + 1) {
            return HostRun(TRUE, ready, background, paths, count);
        }
        return HostRun(FALSE, NULL, background, paths, count);
    }
}

__declspec(noreturn) void QuickPadEntry(void)
{
    ExitProcess((UINT)QuickPadMain());
}
