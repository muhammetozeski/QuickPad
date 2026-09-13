#pragma once

#include <windows.h>

/*
 * Starting with Windows: a value named QuickPad under the current user's Run key that starts this
 * executable with --background.
 */

/* TRUE when the Run value exists and starts this executable. */
BOOL StartupIsEnabled(void);

/* Adds or removes the Run value; on failure tells the user in a message box owned by owner. */
void StartupToggle(HWND owner);

/* On the first run asks whether QuickPad should start with Windows and remembers that it asked. */
void StartupAskOnce(void);
