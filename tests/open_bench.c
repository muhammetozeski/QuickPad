/*
 * Development tool: measures how long QuickPad takes to show a file.
 *
 *     open_bench <QuickPad.exe> <file> [runs] [--pool N]
 *
 * Each run starts the executable with the file and waits for a top-level window whose title holds
 * the file name to become visible (shown or uncloaked, and not cloaked). It then asks that window
 * to close and waits until it is hidden or cloaked again. With --pool N the tool first waits until
 * N QuickPad editor windows exist, so the runs measure a warm pool.
 */
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define EDITOR_CLASS L"QuickPadEditor"

static LARGE_INTEGER frequency;
static const wchar_t *fileName;
static HWND shownWindow;
static double shownAt;
static double createdAt;
static BOOL goneAgain;

static double Now(void)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static BOOL IsCloaked(HWND window)
{
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0;
}

static void CALLBACK OnEvent(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time)
{
    double at = Now();
    (void)hook;
    (void)thread;
    (void)time;
    if (window == NULL || object != OBJID_WINDOW || child != CHILDID_SELF || GetAncestor(window, GA_ROOT) != window) {
        return;
    }

    if (event == EVENT_OBJECT_CREATE) {
        wchar_t className[64];
        if (createdAt == 0 && GetClassNameW(window, className, 64) > 0 && wcscmp(className, EDITOR_CLASS) == 0) {
            createdAt = at;
        }
    } else if (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED) {
        wchar_t title[512];
        InternalGetWindowText(window, title, 512);
        if (shownWindow == NULL && wcsstr(title, fileName) != NULL && IsWindowVisible(window) && !IsCloaked(window)) {
            shownWindow = window;
            shownAt = at;
        }
    } else if ((event == EVENT_OBJECT_HIDE || event == EVENT_OBJECT_CLOAKED || event == EVENT_OBJECT_DESTROY) && window == shownWindow) {
        goneAgain = TRUE;
    }
}

static BOOL PumpUntil(BOOL (*done)(void), DWORD timeout)
{
    DWORD start = GetTickCount();
    while (!done()) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= timeout) {
            return FALSE;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, timeout - elapsed, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            DispatchMessageW(&message);
        }
    }
    return TRUE;
}

static BOOL WindowShown(void)
{
    return shownWindow != NULL;
}

static BOOL WindowGone(void)
{
    return goneAgain;
}

static int editorCount;

static BOOL CALLBACK CountEditor(HWND window, LPARAM parameter)
{
    (void)parameter;
    wchar_t className[64];
    if (GetClassNameW(window, className, 64) > 0 && wcscmp(className, EDITOR_CLASS) == 0) {
        ++editorCount;
    }
    return TRUE;
}

static int CountEditors(void)
{
    editorCount = 0;
    EnumWindows(CountEditor, 0);
    return editorCount;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fwprintf(stderr, L"usage: open_bench <QuickPad.exe> <file> [runs] [--pool N]\n");
        return 2;
    }
    QueryPerformanceFrequency(&frequency);
    const wchar_t *executable = argv[1];
    const wchar_t *file = argv[2];
    int runs = argc > 3 && argv[3][0] != L'-' ? _wtoi(argv[3]) : 5;
    int pool = 0;
    for (int i = 3; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--pool") == 0) {
            pool = _wtoi(argv[i + 1]);
        }
    }
    fileName = wcsrchr(file, L'\\') != NULL ? wcsrchr(file, L'\\') + 1 : file;

    if (pool > 0) {
        DWORD start = GetTickCount();
        while (CountEditors() < pool && GetTickCount() - start < 60000) {
            Sleep(50);
        }
        wprintf(L"pool: %d editor windows after %lu ms\n", CountEditors(), GetTickCount() - start);
    }

    HWINEVENTHOOK showHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, NULL, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK cloakHook = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, NULL, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT);

    double total = 0;
    int measured = 0;
    for (int run = 0; run < runs; ++run) {
        shownWindow = NULL;
        goneAgain = FALSE;
        createdAt = 0;

        size_t commandLength = wcslen(executable) + wcslen(file) + 8;
        wchar_t *commandLine = malloc(commandLength * sizeof(wchar_t));
        swprintf(commandLine, commandLength, L"\"%s\" \"%s\"", executable, file);
        STARTUPINFOW startup = { sizeof startup };
        PROCESS_INFORMATION process;
        double started = Now();
        if (!CreateProcessW(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
            fwprintf(stderr, L"CreateProcess failed: %lu\n", GetLastError());
            return 1;
        }
        free(commandLine);

        BOOL shown = PumpUntil(WindowShown, 10000);
        double launcherExit = -1;
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            FILETIME creation, exit, kernel, user;
            GetProcessTimes(process.hProcess, &creation, &exit, &kernel, &user);
            ULARGE_INTEGER a = { .LowPart = creation.dwLowDateTime, .HighPart = creation.dwHighDateTime };
            ULARGE_INTEGER b = { .LowPart = exit.dwLowDateTime, .HighPart = exit.dwHighDateTime };
            launcherExit = (double)(b.QuadPart - a.QuadPart) / 10000.0;
        }
        if (!shown) {
            wprintf(L"run %d: no window within 10 s\n", run + 1);
        } else {
            double elapsed = shownAt - started;
            wchar_t created[48] = L"no window created";
            if (createdAt != 0) {
                swprintf(created, 48, L"window created at %.2f ms", createdAt - started);
            }
            if (launcherExit >= 0) {
                wprintf(L"run %d: visible after %.2f ms (%s, launcher process lived %.1f ms)\n", run + 1, elapsed, created, launcherExit);
            } else {
                wprintf(L"run %d: visible after %.2f ms (%s, launcher still running)\n", run + 1, elapsed, created);
            }
            total += elapsed;
            ++measured;

            DWORD_PTR result;
            SendMessageTimeoutW(shownWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 10000, &result);
            PostMessageW(shownWindow, WM_CLOSE, 0, 0);
            PumpUntil(WindowGone, 10000);
        }
        WaitForSingleObject(process.hProcess, 10000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (pool > 0) {
            DWORD start = GetTickCount();
            while (CountEditors() < pool && GetTickCount() - start < 10000) {
                Sleep(20);
            }
        } else {
            Sleep(300);
        }
    }

    if (measured > 0) {
        wprintf(L"average: %.2f ms over %d runs\n", total / measured, measured);
    }
    UnhookWinEvent(showHook);
    UnhookWinEvent(cloakHook);
    return 0;
}
