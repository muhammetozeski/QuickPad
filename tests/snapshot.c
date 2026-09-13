/*
 * Development tool: saves a window, found by its exact title, to a 32-bit BMP file with PrintWindow.
 * With "close" as the third argument the window is sent WM_CLOSE afterwards.
 *
 *     snapshot "notes.txt - QuickPad" notes.bmp [close]
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 3) {
        fwprintf(stderr, L"usage: snapshot <window title> <output.bmp> [close]\n");
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

    RECT rect;
    GetWindowRect(window, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    BITMAPINFO info = { 0 };
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    HDC dc = CreateCompatibleDC(screen);
    void *bits = NULL;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    PrintWindow(window, dc, PW_RENDERFULLCONTENT);

    BITMAPFILEHEADER header = { 0 };
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + (DWORD)width * (DWORD)height * 4;

    FILE *file = NULL;
    if (_wfopen_s(&file, argv[2], L"wb") != 0 || file == NULL) {
        fwprintf(stderr, L"cannot write %s\n", argv[2]);
        return 1;
    }
    fwrite(&header, sizeof header, 1, file);
    fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, file);
    fwrite(bits, 4, (size_t)width * (size_t)height, file);
    fclose(file);

    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);

    if (argc > 3 && wcscmp(argv[3], L"close") == 0) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return 0;
}
