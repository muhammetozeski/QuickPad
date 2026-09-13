#pragma once

#include <windows.h>

/* Registers the window classes and loads what every editor window shares. */
BOOL EditorInitialize(HINSTANCE instance);

/* A resident process keeps running when its last editor window closes; otherwise it quits. */
void EditorSetResident(BOOL resident);

/* Opens a full path in its own window, or brings forward the window that already shows it. */
BOOL EditorOpenFile(const wchar_t *path);
BOOL EditorOpenNew(void);

/* Closes every window, offering to save changes first; FALSE when the user cancels. */
BOOL EditorCloseAll(void);

int EditorShownCount(void);

/* Handles keyboard shortcuts for editor windows; TRUE when the message was used. */
BOOL EditorTranslateMessage(MSG *message);
