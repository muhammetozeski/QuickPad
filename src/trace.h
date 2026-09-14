#pragma once

#include <windows.h>

/*
 * Timestamps for measuring the steps of opening and closing a window. They exist only in a build
 * with QP_TRACE defined (see obj\bench\build_trace.ps1), which appends them to a text file; the
 * regular build compiles them away.
 */
#ifdef QP_TRACE
void TraceMark(const char *label);
void TraceDump(const char *title);
#define TRACE(label) TraceMark(label)
#define TRACE_DUMP(title) TraceDump(title)
#else
#define TRACE(label) ((void)0)
#define TRACE_DUMP(title) ((void)0)
#endif
