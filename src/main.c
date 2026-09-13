#include "quickpad.h"

static LRESULT CALLBACK MainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static int QuickPadMain(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);

    WNDCLASSEXW windowClass = { sizeof windowClass };
    windowClass.lpfnWndProc = MainWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(INT_PTR)(COLOR_WINDOW + 1);
    windowClass.lpszClassName = L"QuickPadEditor";
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    HWND window = CreateWindowExW(0, windowClass.lpszClassName, QP_APP_NAME, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, NULL, NULL, instance, NULL);
    if (window == NULL) {
        return 1;
    }
    ShowWindow(window, SW_SHOWDEFAULT);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return (int)message.wParam;
}

__declspec(noreturn) void QuickPadEntry(void)
{
    ExitProcess((UINT)QuickPadMain());
}
