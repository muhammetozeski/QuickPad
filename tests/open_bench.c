/*
 * Development tool: measures how long QuickPad takes to show a file.
 *
 *     open_bench <QuickPad.exe> <file> [runs] [--pool N] [--interval ms] [--shell | --com | --dll path]
 *
 * Each run starts the executable with the file and waits for a top-level window whose title holds
 * the file name to become visible (shown or uncloaked, and not cloaked). It then asks that window
 * to close and waits until it is hidden or cloaked again. With --pool N the tool first waits until
 * N QuickPad editor windows exist, so the runs measure a warm pool. --interval waits that many
 * milliseconds between runs instead of waiting for the pool to be full again.
 *
 * --shell opens the file through ShellExecuteExW and its association. Use it only for a file type
 * QuickPad is already the default for: Windows asks the user to pick an app for any other type.
 * --com does what Explorer does once the association names QuickPad's open command: it loads the
 * command from QuickPadShell.dll through COM, hands it the file and executes it, without touching
 * any association. QuickPad.exe --register must have been run. --dll <path> does the same with the
 * class factory of the given QuickPadShell.dll, loaded directly, so no registration is needed.
 */
#define COBJMACROS

#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define EDITOR_CLASS L"QuickPadEditor"

/* QUICKPAD_OPEN_COMMAND_CLSID from src/shellid.h. */
static const CLSID openCommandClass = { 0x0DAFC6E1, 0xFF1B, 0x4AA1, { 0x9E, 0x0F, 0xBF, 0x6D, 0x3F, 0xAC, 0xA6, 0xDB } };

static LARGE_INTEGER frequency;
static const wchar_t *fileName;
static HWND shownWindow;
static double shownAt;
static double createdAt;
static double titledAt;
static double movedAt;
static double foregroundAt;
static HWND titledWindow;
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
    if (event == EVENT_SYSTEM_FOREGROUND) {
        if (window != NULL && window == titledWindow && foregroundAt == 0) {
            foregroundAt = at;
        }
        return;
    }
    if (window == NULL || object != OBJID_WINDOW || child != CHILDID_SELF || GetAncestor(window, GA_ROOT) != window) {
        return;
    }

    if (event == EVENT_OBJECT_NAMECHANGE) {
        wchar_t title[512];
        InternalGetWindowText(window, title, 512);
        if (titledAt == 0 && wcsstr(title, fileName) != NULL) {
            titledAt = at;
            titledWindow = window;
        }
    } else if (event == EVENT_OBJECT_LOCATIONCHANGE) {
        if (window == titledWindow && movedAt == 0) {
            movedAt = at;
        }
    } else if (event == EVENT_OBJECT_CREATE) {
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
    BOOL shell = FALSE;
    BOOL com = FALSE;
    const wchar_t *dllPath = NULL;
    int interval = -1;
    for (int i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--pool") == 0 && i + 1 < argc) {
            pool = _wtoi(argv[i + 1]);
        } else if (wcscmp(argv[i], L"--shell") == 0) {
            shell = TRUE;
        } else if (wcscmp(argv[i], L"--com") == 0) {
            com = TRUE;
        } else if (wcscmp(argv[i], L"--interval") == 0 && i + 1 < argc) {
            interval = _wtoi(argv[i + 1]);
        } else if (wcscmp(argv[i], L"--dll") == 0 && i + 1 < argc) {
            dllPath = argv[i + 1];
            com = TRUE;
        }
    }
    HMODULE shellModule = NULL;
    LPFNGETCLASSOBJECT getClassObject = NULL;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
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
    HWINEVENTHOOK nameHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_NAMECHANGE, NULL, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT);

    double total = 0;
    int measured = 0;
    for (int run = 0; run < runs; ++run) {
        shownWindow = NULL;
        goneAgain = FALSE;
        createdAt = 0;
        titledAt = 0;
        movedAt = 0;
        foregroundAt = 0;
        titledWindow = NULL;

        PROCESS_INFORMATION process = { 0 };
        IShellItemArray *items = NULL;
        if (com) {
            IShellItem *item = NULL;
            if (FAILED(SHCreateItemFromParsingName(file, NULL, &IID_IShellItem, (void **)&item))
                || FAILED(SHCreateShellItemArrayFromShellItem(item, &IID_IShellItemArray, (void **)&items))) {
                fwprintf(stderr, L"cannot create a shell item for %s\n", file);
                return 1;
            }
            IShellItem_Release(item);
        }
        double started = Now();
        if (com) {
            IExecuteCommand *command = NULL;
            HRESULT result = E_FAIL;
            if (dllPath != NULL) {
                if (shellModule == NULL) {
                    shellModule = LoadLibraryW(dllPath);
                    getClassObject = shellModule != NULL ? (LPFNGETCLASSOBJECT)GetProcAddress(shellModule, "DllGetClassObject") : NULL;
                }
                IClassFactory *factory = NULL;
                if (getClassObject != NULL && SUCCEEDED(result = getClassObject(&openCommandClass, &IID_IClassFactory, (void **)&factory))) {
                    result = IClassFactory_CreateInstance(factory, NULL, &IID_IExecuteCommand, (void **)&command);
                    IClassFactory_Release(factory);
                }
            } else {
                result = CoCreateInstance(&openCommandClass, NULL, CLSCTX_INPROC_SERVER, &IID_IExecuteCommand, (void **)&command);
            }
            if (FAILED(result)) {
                fwprintf(stderr, L"CoCreateInstance failed: 0x%08lx\n", (unsigned long)result);
                return 1;
            }
            double created = Now();
            IObjectWithSelection *selection = NULL;
            if (SUCCEEDED(IExecuteCommand_QueryInterface(command, &IID_IObjectWithSelection, (void **)&selection))) {
                IObjectWithSelection_SetSelection(selection, items);
                IObjectWithSelection_Release(selection);
            }
            double selected = Now();
            result = IExecuteCommand_Execute(command);
            double executed = Now();
            IExecuteCommand_Release(command);
            IShellItemArray_Release(items);
            wprintf(L"run %d: COM create %.2f, selection %.2f, execute 0x%08lx %.2f, release %.2f ms\n", run + 1,
                created - started, selected - created, (unsigned long)result, executed - selected, Now() - executed);
        } else if (shell) {
            SHELLEXECUTEINFOW info = { sizeof info };
            info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
            info.lpVerb = L"open";
            info.lpFile = file;
            info.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&info)) {
                fwprintf(stderr, L"ShellExecuteEx failed: %lu\n", GetLastError());
                return 1;
            }
            wprintf(L"run %d: ShellExecuteEx returned after %.2f ms%s\n", run + 1, Now() - started,
                info.hProcess != NULL ? L" with a new process" : L" without a new process");
            process.hProcess = info.hProcess;
        } else {
            size_t commandLength = wcslen(executable) + wcslen(file) + 8;
            wchar_t *commandLine = malloc(commandLength * sizeof(wchar_t));
            swprintf(commandLine, commandLength, L"\"%s\" \"%s\"", executable, file);
            STARTUPINFOW startup = { sizeof startup };
            if (!CreateProcessW(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
                fwprintf(stderr, L"CreateProcess failed: %lu\n", GetLastError());
                return 1;
            }
            free(commandLine);
        }

        BOOL shown = PumpUntil(WindowShown, 10000);
        double launcherExit = -1;
        if (process.hProcess != NULL && WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
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
            if (titledAt != 0) {
                wprintf(L"        title set %.2f, moved %.2f, foreground %.2f, visible %.2f ms\n", titledAt - started,
                    movedAt != 0 ? movedAt - started : -1.0, foregroundAt != 0 ? foregroundAt - started : -1.0, elapsed);
            }
            total += elapsed;
            ++measured;

            DWORD_PTR result;
            SendMessageTimeoutW(shownWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 10000, &result);
            PostMessageW(shownWindow, WM_CLOSE, 0, 0);
            PumpUntil(WindowGone, 10000);
        }
        if (process.hProcess != NULL) {
            WaitForSingleObject(process.hProcess, 10000);
            CloseHandle(process.hProcess);
        }
        if (process.hThread != NULL) {
            CloseHandle(process.hThread);
        }
        if (interval >= 0) {
            Sleep((DWORD)interval);
        } else if (pool > 0) {
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
    UnhookWinEvent(nameHook);
    UnhookWinEvent(foregroundHook);
    return 0;
}
