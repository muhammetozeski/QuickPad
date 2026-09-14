#include "trace.h"

#ifdef QP_TRACE
#define TRACE_CAPACITY 64
#define TRACE_FILE L"QuickPad-trace.txt"

typedef struct TraceEntry {
    const char *label;
    LONGLONG ticks;
} TraceEntry;

static TraceEntry traceEntries[TRACE_CAPACITY];
static int traceCount;

void TraceMark(const char *label)
{
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    if (traceCount < TRACE_CAPACITY) {
        traceEntries[traceCount].label = label;
        traceEntries[traceCount].ticks = counter.QuadPart;
        ++traceCount;
    }
}

/* Writes the marks since the last dump, as microseconds from the first mark, next to the executable. */
void TraceDump(const char *title)
{
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (length > 0 && path[length - 1] != L'\\') {
        --length;
    }
    lstrcpynW(path + length, TRACE_FILE, MAX_PATH - (int)length);

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        traceCount = 0;
        return;
    }
    char line[256];
    DWORD written = 0;
    int lineLength = wsprintfA(line, "=== %s\r\n", title);
    WriteFile(file, line, (DWORD)lineLength, &written, NULL);
    LONGLONG start = traceCount > 0 ? traceEntries[0].ticks : 0;
    LONGLONG previous = start;
    for (int i = 0; i < traceCount; ++i) {
        LONGLONG at = (traceEntries[i].ticks - start) * 1000000 / frequency.QuadPart;
        LONGLONG step = (traceEntries[i].ticks - previous) * 1000000 / frequency.QuadPart;
        lineLength = wsprintfA(line, "%-24s at %8d us  (+%6d us)\r\n", traceEntries[i].label, (int)at, (int)step);
        WriteFile(file, line, (DWORD)lineLength, &written, NULL);
        previous = traceEntries[i].ticks;
    }
    CloseHandle(file);
    traceCount = 0;
}
#endif
