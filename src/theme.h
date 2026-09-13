#pragma once

#include <windows.h>

#include "textview.h"

/* QuickPad always uses dark colors. */

/* Makes popup menus of this process dark. */
void ThemeInitialize(void);

/* Dark title bar for a top-level window. */
void ThemePrepareWindow(HWND window);

/* Dark scroll bars for a window with WS_VSCROLL or WS_HSCROLL. */
void ThemePrepareScrollBars(HWND window);

const TextViewColors *ThemeTextColors(void);

/* Dark title bar, buttons and edit boxes for a dialog; call from WM_INITDIALOG. */
void ThemePrepareDialog(HWND dialog);

/* Dark backgrounds for dialog controls. Returns TRUE with *result set when the message was handled. */
BOOL ThemeDialogMessage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam, INT_PTR *result);

/*
 * Draws the menu bar of a top-level window in dark colors. Call it first from the window procedure;
 * when it returns TRUE the message is handled and *result holds the value to return.
 */
BOOL ThemeMenuBarMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam, LRESULT *result);
