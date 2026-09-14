#pragma once

#include <windows.h>

/* A trace build (see trace.h) runs beside the regular host under its own names. */
#ifdef QP_TRACE
#define HOST_WINDOW_CLASS L"QuickPadTraceHost"
#define HOST_MUTEX_NAME L"Local\\QuickPadTrace.Host"
#define HOST_READY_EVENT_NAME L"Local\\QuickPadTrace.HostReady"
#else
#define HOST_WINDOW_CLASS L"QuickPadHost"
#define HOST_MUTEX_NAME L"Local\\QuickPad.Host"
#define HOST_READY_EVENT_NAME L"Local\\QuickPad.HostReady"
#endif

/* dwData of WM_COPYDATA sent to the host window. */
#define HOST_COPY_OPEN 0x51500001 /* lpData: a null-terminated full path */
#define HOST_COPY_NEW 0x51500002  /* opens an empty window */

/* Posted to the host window: lParam is a MemAlloc full path the host opens and frees, or NULL for an empty window. */
#define WM_HOST_OPEN (WM_APP + 1)

/*
 * Runs the editor in this process. A resident host keeps running after its last window closes,
 * shows a notification area icon and receives the files of later launches; readyEvent is set once
 * its window exists. A process that is not resident ends with its last window.
 */
int HostRun(BOOL resident, HANDLE readyEvent, BOOL background, wchar_t **paths, size_t count);
