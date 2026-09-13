#pragma once

#include <windows.h>

#define HOST_WINDOW_CLASS L"QuickPadHost"
#define HOST_MUTEX_NAME L"Local\\QuickPad.Host"
#define HOST_READY_EVENT_NAME L"Local\\QuickPad.HostReady"

/* dwData of WM_COPYDATA sent to the host window. */
#define HOST_COPY_OPEN 0x51500001 /* lpData: a null-terminated full path */
#define HOST_COPY_NEW 0x51500002  /* opens an empty window */

/*
 * Runs the editor in this process. A resident host keeps running after its last window closes,
 * shows a notification area icon and receives the files of later launches; readyEvent is set once
 * its window exists. A process that is not resident ends with its last window.
 */
int HostRun(BOOL resident, HANDLE readyEvent, BOOL background, wchar_t **paths, size_t count);
