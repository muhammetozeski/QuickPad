#include "theme.h"
#include "quickpad.h"

#include <dwmapi.h>
#include <uxtheme.h>

#define MENU_BAR_COLOR RGB(32, 32, 32)
#define MENU_HOT_COLOR RGB(61, 61, 61)
#define MENU_PRESSED_COLOR RGB(80, 80, 80)
#define MENU_TEXT_COLOR RGB(230, 230, 230)
#define MENU_INACTIVE_TEXT_COLOR RGB(170, 170, 170)
#define MENU_DISABLED_TEXT_COLOR RGB(120, 120, 120)

/*
 * Windows draws the menu bar itself and has no dark version of it. It sends these undocumented
 * messages before drawing, and a window that handles them draws the bar instead.
 */
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092

typedef struct UahMenu {
    HMENU menu;
    HDC dc;
    DWORD flags;
} UahMenu;

typedef union UahMenuItemMetrics {
    struct {
        DWORD cx;
        DWORD cy;
    } bar[2];
    struct {
        DWORD cx;
        DWORD cy;
    } popup[4];
} UahMenuItemMetrics;

typedef struct UahMenuPopupMetrics {
    DWORD widths[4];
    DWORD updateMaxWidths : 2;
} UahMenuPopupMetrics;

typedef struct UahMenuItem {
    int position;
    UahMenuItemMetrics metrics;
    UahMenuPopupMetrics popupMetrics;
} UahMenuItem;

typedef struct UahDrawMenuItem {
    DRAWITEMSTRUCT draw;
    UahMenu menu;
    UahMenuItem item;
} UahDrawMenuItem;

static const TextViewColors darkText = {
    RGB(220, 220, 220),
    RGB(30, 30, 30),
    RGB(255, 255, 255),
    RGB(38, 79, 120),
    RGB(58, 61, 65),
};

static HBRUSH menuBarBrush;
static HBRUSH menuHotBrush;
static HBRUSH menuPressedBrush;

void ThemeInitialize(void)
{
    menuBarBrush = CreateSolidBrush(MENU_BAR_COLOR);
    menuHotBrush = CreateSolidBrush(MENU_HOT_COLOR);
    menuPressedBrush = CreateSolidBrush(MENU_PRESSED_COLOR);

    /* Popup menus follow the app mode set through undocumented uxtheme exports 135 and 136. */
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (uxtheme != NULL) {
        int (WINAPI *setPreferredAppMode)(int) = (int (WINAPI *)(int))GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
        void (WINAPI *flushMenuThemes)(void) = (void (WINAPI *)(void))GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
        if (setPreferredAppMode != NULL) {
            const int forceDark = 2;
            setPreferredAppMode(forceDark);
        }
        if (flushMenuThemes != NULL) {
            flushMenuThemes();
        }
    }
}

void ThemePrepareWindow(HWND window)
{
    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
}

void ThemePrepareScrollBars(HWND window)
{
    SetWindowTheme(window, L"DarkMode_Explorer", NULL);
}

const TextViewColors *ThemeTextColors(void)
{
    return &darkText;
}

/* Covers the light line Windows draws between the menu bar and the client area. */
static void PaintMenuBarEdge(HWND window)
{
    MENUBARINFO info = { sizeof info };
    if (!GetMenuBarInfo(window, OBJID_MENU, 0, &info)) {
        return;
    }
    RECT client;
    GetClientRect(window, &client);
    MapWindowPoints(window, NULL, (POINT *)&client, 2);
    RECT frame;
    GetWindowRect(window, &frame);
    OffsetRect(&client, -frame.left, -frame.top);

    RECT edge = client;
    edge.bottom = edge.top;
    edge.top -= 1;
    HDC dc = GetWindowDC(window);
    FillRect(dc, &edge, menuBarBrush);
    ReleaseDC(window, dc);
}

BOOL ThemeMenuBarMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT *result)
{
    switch (message) {
    case WM_UAHDRAWMENU: {
        UahMenu *menu = (UahMenu *)lParam;
        MENUBARINFO info = { sizeof info };
        if (!GetMenuBarInfo(window, OBJID_MENU, 0, &info)) {
            return FALSE;
        }
        RECT frame;
        GetWindowRect(window, &frame);
        RECT bar = info.rcBar;
        OffsetRect(&bar, -frame.left, -frame.top);
        FillRect(menu->dc, &bar, menuBarBrush);
        *result = TRUE;
        return TRUE;
    }

    case WM_UAHDRAWMENUITEM: {
        UahDrawMenuItem *item = (UahDrawMenuItem *)lParam;
        wchar_t text[128];
        MENUITEMINFOW info = { sizeof info };
        info.fMask = MIIM_STRING;
        info.dwTypeData = text;
        info.cch = ARRAYSIZE(text) - 1;
        if (!GetMenuItemInfoW(item->menu.menu, (UINT)item->item.position, TRUE, &info)) {
            return FALSE;
        }

        UINT state = item->draw.itemState;
        HBRUSH brush = (state & ODS_SELECTED) != 0 ? menuPressedBrush
            : (state & ODS_HOTLIGHT) != 0 ? menuHotBrush
            : menuBarBrush;
        COLORREF color = (state & (ODS_GRAYED | ODS_DISABLED)) != 0 ? MENU_DISABLED_TEXT_COLOR
            : (state & ODS_INACTIVE) != 0 ? MENU_INACTIVE_TEXT_COLOR
            : MENU_TEXT_COLOR;
        FillRect(item->menu.dc, &item->draw.rcItem, brush);
        SetBkMode(item->menu.dc, TRANSPARENT);
        SetTextColor(item->menu.dc, color);
        UINT format = DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((state & ODS_NOACCEL) != 0 ? DT_HIDEPREFIX : 0);
        DrawTextW(item->menu.dc, text, (int)info.cch, &item->draw.rcItem, format);
        *result = TRUE;
        return TRUE;
    }

    case WM_NCPAINT:
    case WM_NCACTIVATE:
        *result = DefWindowProcW(window, message, wParam, lParam);
        PaintMenuBarEdge(window);
        return TRUE;
    }
    return FALSE;
}
