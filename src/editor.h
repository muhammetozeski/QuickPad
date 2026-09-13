#pragma once

#include <windows.h>

/* Registers the window classes and loads what every editor window shares. */
BOOL EditorInitialize(HINSTANCE instance);

/*
 * With a host window the process is resident: it keeps running when its last editor window closes,
 * and pooled windows are owned by the host. Without one the process quits with its last window.
 */
void EditorSetHost(HWND host);

/* Opens a full path in its own window, or brings forward the window that already shows it. */
BOOL EditorOpenFile(const wchar_t *path);
BOOL EditorOpenNew(void);

/*
 * Keeps the pool of drawn, cloaked editor windows at its size, one window per call. Call it when the
 * message queue is empty. It returns FALSE when there is nothing to do now; *wait is then how many
 * milliseconds may pass before it has work again, or INFINITE.
 */
BOOL EditorIdle(DWORD *wait);

int EditorPoolSize(void);
void EditorSetPoolSize(int size);

/* Closes every window, offering to save changes first; FALSE when the user cancels. */
BOOL EditorCloseAll(void);

int EditorShownCount(void);

/* Handles keyboard shortcuts for editor windows; TRUE when the message was used. */
BOOL EditorTranslateMessage(MSG *message);
