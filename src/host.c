#include "host.h"
#include "editor.h"
#include "quickpad.h"
#include "resource.h"

#include <shellapi.h>
#include <windowsx.h>

#define WM_HOST_OPEN (WM_APP + 1)
#define WM_HOST_TRAY (WM_APP + 2)
#define TRAY_ICON_ID 1
#define MAX_PATH_BYTES (32768 * sizeof(wchar_t))

static HWND hostWindow;
static HANDLE hostReadyEvent;
static UINT taskbarCreatedMessage;

static void AddTrayIcon(void)
{
    NOTIFYICONDATAW data = { sizeof data };
    data.hWnd = hostWindow;
    data.uID = TRAY_ICON_ID;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = WM_HOST_TRAY;
    data.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(data.szTip, QP_APP_NAME, ARRAYSIZE(data.szTip));
    Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

static void RemoveTrayIcon(void)
{
    NOTIFYICONDATAW data = { sizeof data };
    data.hWnd = hostWindow;
    data.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

static void ExitHost(void)
{
    if (!EditorCloseAll()) {
        return;
    }
    RemoveTrayIcon();
    ResetEvent(hostReadyEvent);
    DestroyWindow(hostWindow);
}

static void ShowTrayMenu(int x, int y)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_NEW, L"&New Window");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"E&xit");

    /* The menu only closes on an outside click when its owner is the foreground window. */
    SetForegroundWindow(hostWindow);
    UINT command = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hostWindow, NULL);
    PostMessageW(hostWindow, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command == IDM_TRAY_NEW) {
        EditorOpenNew();
    } else if (command == IDM_TRAY_EXIT) {
        ExitHost();
    }
}

static LRESULT CALLBACK HostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_COPYDATA: {
        const COPYDATASTRUCT *data = (const COPYDATASTRUCT *)lParam;
        if (data->dwData == HOST_COPY_NEW) {
            return PostMessageW(window, WM_HOST_OPEN, 0, 0);
        }
        if (data->dwData != HOST_COPY_OPEN || data->lpData == NULL || data->cbData < 2 * sizeof(wchar_t)
            || data->cbData % sizeof(wchar_t) != 0 || data->cbData > MAX_PATH_BYTES) {
            return FALSE;
        }
        /* The sender waits for this message, so the file is opened after it returns. */
        wchar_t *path = MemAlloc(data->cbData);
        if (path == NULL) {
            return FALSE;
        }
        memcpy(path, data->lpData, data->cbData);
        path[data->cbData / sizeof(wchar_t) - 1] = 0;
        if (!PostMessageW(window, WM_HOST_OPEN, 0, (LPARAM)path)) {
            MemFree(path);
            return FALSE;
        }
        return TRUE;
    }

    case WM_HOST_OPEN: {
        wchar_t *path = (wchar_t *)lParam;
        if (path != NULL) {
            EditorOpenFile(path);
            MemFree(path);
        } else {
            EditorOpenNew();
        }
        return 0;
    }

    case WM_HOST_TRAY:
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            EditorOpenNew();
            break;
        case WM_CONTEXTMENU:
            ShowTrayMenu(GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
            break;
        }
        return 0;

    case WM_CLOSE:
        ExitHost();
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    if (message == taskbarCreatedMessage && message != 0) {
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static BOOL CreateHostWindow(HINSTANCE instance)
{
    WNDCLASSEXW windowClass = { sizeof windowClass };
    windowClass.lpfnWndProc = HostProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = HOST_WINDOW_CLASS;
    if (!RegisterClassExW(&windowClass)) {
        return FALSE;
    }
    hostWindow = CreateWindowExW(WS_EX_TOOLWINDOW, HOST_WINDOW_CLASS, QP_APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL,
        instance, NULL);
    if (hostWindow == NULL) {
        return FALSE;
    }

    /* Launches from a lower integrity level can still hand over their files. */
    taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hostWindow, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(hostWindow, taskbarCreatedMessage, MSGFLT_ALLOW, NULL);
    AddTrayIcon();
    return TRUE;
}

int HostRun(BOOL resident, HANDLE readyEvent, BOOL background, wchar_t **paths, size_t count)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    if (resident) {
        ResetEvent(readyEvent);
    }
    if (!EditorInitialize(instance)) {
        return 1;
    }
    if (resident && CreateHostWindow(instance)) {
        hostReadyEvent = readyEvent;
        SetEvent(readyEvent);
    } else {
        resident = FALSE;
    }
    EditorSetResident(resident);

    for (size_t i = 0; i < count; ++i) {
        EditorOpenFile(paths[i]);
    }
    if (count == 0 && !background) {
        EditorOpenNew();
    }
    if (!resident && EditorShownCount() == 0) {
        return 0;
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
