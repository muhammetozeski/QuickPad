#pragma once

#include <windows.h>

/* Registers the window classes and loads what every editor window shares. */
BOOL EditorInitialize(HINSTANCE instance);

/* Opens a full path in its own window, or brings forward the window that already shows it. */
BOOL EditorOpenFile(const wchar_t *path);
BOOL EditorOpenNew(void);

/* Handles keyboard shortcuts for editor windows; TRUE when the message was used. */
BOOL EditorTranslateMessage(MSG *message);
