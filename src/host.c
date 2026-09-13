#include "host.h"
#include "editor.h"
#include "quickpad.h"
#include "resource.h"
#include "settings.h"
#include "startup.h"
#include "theme.h"

#include <shellapi.h>
#include <windowsx.h>

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
    data.hIcon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), 0);
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

static INT_PTR CALLBACK PoolSizeProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    INT_PTR themed = 0;
    if (ThemeDialogMessage(dialog, message, wParam, lParam, &themed)) {
        return themed;
    }

    switch (message) {
    case WM_INITDIALOG:
        ThemePrepareDialog(dialog);
        SendDlgItemMessageW(dialog, IDC_POOL_SIZE, EM_LIMITTEXT, 3, 0);
        SetDlgItemInt(dialog, IDC_POOL_SIZE, (UINT)EditorPoolSize(), FALSE);
        SetForegroundWindow(dialog);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            BOOL valid = FALSE;
            UINT size = GetDlgItemInt(dialog, IDC_POOL_SIZE, &valid, FALSE);
            if (!valid || size > SETTINGS_POOL_SIZE_MAX) {
                MessageBeep(MB_ICONWARNING);
                HWND edit = GetDlgItem(dialog, IDC_POOL_SIZE);
                SetFocus(edit);
                SendMessageW(edit, EM_SETSEL, 0, -1);
                return TRUE;
            }
            EditorSetPoolSize((int)size);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void ShowTrayMenu(int x, int y)
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_NEW, L"&New Window");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_POOL_SIZE, L"&Window Pool Size...");
    AppendMenuW(menu, MF_STRING | (StartupIsEnabled() ? MF_CHECKED : MF_UNCHECKED), IDM_START_WITH_WINDOWS, L"&Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"E&xit");

    /* The menu only closes on an outside click when its owner is the foreground window. */
    SetForegroundWindow(hostWindow);
    UINT command = (UINT)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0, hostWindow, NULL);
    PostMessageW(hostWindow, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (command != 0) {
        PostMessageW(hostWindow, WM_COMMAND, command, 0);
    }
}

static void HandleHostCommand(UINT command)
{
    switch (command) {
    case IDM_TRAY_NEW:
        EditorOpenNew();
        break;
    case IDM_TRAY_POOL_SIZE:
        DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_POOL_SIZE), NULL, PoolSizeProc, 0);
        break;
    case IDM_START_WITH_WINDOWS:
        StartupToggle(NULL);
        break;
    case IDM_TRAY_EXIT:
        ExitHost();
        break;
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

    case WM_COMMAND:
        HandleHostCommand(LOWORD(wParam));
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

/*
 * The host sleeps nearly all the time and has to answer at once when woken, so it asks Windows not to
 * treat it as background work: no power throttling (EcoQoS), a priority above normal, and a working set
 * Windows does not trim below 64 MB, which keeps its code and pooled windows in memory while idle.
 * Each request is a hint; a system that refuses one simply runs the host without it.
 */
static void RequestResponsiveness(void)
{
    HANDLE process = GetCurrentProcess();
    PROCESS_POWER_THROTTLING_STATE throttling = { PROCESS_POWER_THROTTLING_CURRENT_VERSION };
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0;
    SetProcessInformation(process, ProcessPowerThrottling, &throttling, sizeof throttling);
    SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS);
    SetProcessWorkingSetSizeEx(process, 64 * 1024 * 1024, 1024 * 1024 * 1024,
        QUOTA_LIMITS_HARDWS_MIN_ENABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE);
}

int HostRun(BOOL resident, HANDLE readyEvent, BOOL background, wchar_t **paths, size_t count)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    if (resident) {
        ResetEvent(readyEvent);
        RequestResponsiveness();
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
    EditorSetHost(resident ? hostWindow : NULL);

    for (size_t i = 0; i < count; ++i) {
        EditorOpenFile(paths[i]);
    }
    if (count == 0 && !background) {
        EditorOpenNew();
    }
    if (!resident && EditorShownCount() == 0) {
        return 0;
    }
    if (resident) {
        StartupAskOnce();
    }

    /* Pool windows are drawn only while no message waits, so typing and painting never queue behind them. */
    for (;;) {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return (int)message.wParam;
            }
            if (!EditorTranslateMessage(&message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        DWORD wait = INFINITE;
        if (!EditorIdle(&wait)) {
            MsgWaitForMultipleObjectsEx(0, NULL, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
}
