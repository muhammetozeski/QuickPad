/*
 * Development tool: sends a menu command to a window found by its exact title and waits until the
 * window has handled it. With "close" as the third argument the window is sent WM_CLOSE afterwards.
 *
 *     command "notes.txt - QuickPad" 40003 [close]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fwprintf(stderr, L"usage: command <window title> <command id> [close]\n");
        return 2;
    }

    HWND window = NULL;
    for (int attempt = 0; attempt < 50 && window == NULL; ++attempt) {
        window = FindWindowW(NULL, argv[1]);
        if (window == NULL) {
            Sleep(100);
        }
    }
    if (window == NULL) {
        fwprintf(stderr, L"window not found: %s\n", argv[1]);
        return 1;
    }

    DWORD_PTR result = 0;
    WPARAM command = (WPARAM)wcstoul(argv[2], NULL, 10);
    if (!SendMessageTimeoutW(window, WM_COMMAND, command, 0, SMTO_ABORTIFHUNG, 30000, &result)) {
        fwprintf(stderr, L"the window did not handle the command\n");
        return 1;
    }
    if (argc > 3 && wcscmp(argv[3], L"close") == 0) {
        SendMessageTimeoutW(window, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 30000, &result);
    }
    return 0;
}
